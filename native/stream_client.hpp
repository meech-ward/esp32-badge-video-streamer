#pragma once

#include <cstddef>
#include <cstdint>
#include "lwip/err.h"
#include "streamer/protocol.hpp"

struct tcp_pcb;
struct pbuf;

#ifndef NATIVE_TRACE_GAPS
#define NATIVE_TRACE_GAPS 0
#endif
#ifndef NATIVE_JPEG_PSRAM
#define NATIVE_JPEG_PSRAM 0
#endif
#ifndef NATIVE_TRACE_TCP_INGRESS
#define NATIVE_TRACE_TCP_INGRESS 0
#endif
#if NATIVE_TRACE_TCP_INGRESS
#include "tcp_arrival.hpp"
#endif

namespace native_badge {

struct FrameTrace {
    uint16_t credit_source_seq = 0;
    uint32_t credit_queued_us = 0, credit_output_us = 0, credit_ack_us = 0;
    uint32_t header_us = 0, payload_us = 0, payload_gap_us = 0;
#if NATIVE_TRACE_TCP_INGRESS
    TcpArrivalSnapshot ingress;
#endif
};

struct ReceivedFrame {
    const uint8_t* jpeg = nullptr;
    size_t length = 0;
    uint16_t sequence = 0;
    uint32_t session = 0;
    uint32_t lease = 0;
    uint32_t age_us = 0;
    uint32_t receive_us = 0;
    uint32_t received_at_us = 0;  // local payload-completion timestamp
    FrameTrace trace;
};

class StreamClient {
public:
    static constexpr size_t jpeg_capacity = 64 * 1024;
    void init(uint8_t* jpeg, uint8_t quality, uint8_t* second_jpeg = nullptr);
    void poll(uint32_t now_ms, bool network_ready);
    void close();
    bool connected() const { return hello_sent_; }
    bool frame_ready() const { return ready_; }
    uint32_t session() const { return session_; }
    bool take_frame(ReceivedFrame& frame);
    void frame_done();  // after core 1 has stopped reading the JPEG
    bool frame_done(const ReceivedFrame& lease);
    bool prefetch_credit(const ReceivedFrame& lease);
    uint32_t early_credits = 0, late_credits = 0;

    uint32_t frames = 0, statuses = 0, oversized = 0, errors = 0, connects = 0;
    uint32_t last_frame_bytes = 0, max_frame_bytes = 0;
    uint32_t last_status_us = 0;
    uint32_t connection_failures = 0;
#if NATIVE_TRACE_TCP_INGRESS
    void observe_tcp_segment(const tcp_pcb* pcb, uint32_t sequence, uint32_t expected, uint16_t length);
#endif

private:
    static err_t connected_callback(void*, tcp_pcb*, err_t);
    static err_t receive_callback(void*, tcp_pcb*, pbuf*, err_t);
    static void error_callback(void*, err_t);
    static err_t sent_callback(void*, tcp_pcb*, uint16_t);
    bool consume(const uint8_t* bytes, size_t len);
    bool header_complete();
    void payload_complete();
    void disconnected();
    void transmit_pending();
    void trace_credit(uint16_t source_sequence);
    bool owns_lease(const ReceivedFrame& frame) const;

    tcp_pcb* pcb_ = nullptr;
    uint8_t* jpeg_ = nullptr;
    uint8_t hello_[streamer::kHelloSize]{};
    uint8_t header_[streamer::kHeaderSize]{};
    uint8_t status_[streamer::kStatusMax]{};
    streamer::Header meta_;
    size_t header_got_ = 0, payload_got_ = 0;
    bool reading_payload_ = false, skip_ = false;
    bool connecting_ = false, hello_sent_ = false, ready_ = false, in_use_ = false;
    bool output_pending_ = false;
    uint32_t connect_at_ = 0, retry_at_ = 0, last_rx_ = 0;
    uint32_t header_at_us_ = 0;
    unsigned credits_owed_ = 0;
    uint32_t session_ = 0;
    FrameTrace trace_;
    uint32_t tx_written_ = 0, tx_acked_ = 0, credit_end_ = 0, previous_payload_us_ = 0;
    ReceivedFrame frame_;
    ReceivedFrame leased_frame_;
    uint32_t lease_id_ = 0;
#if NATIVE_JPEG_PSRAM
    uint8_t* inputs_[2]{};
    unsigned rx_slot_ = 0, leased_slot_ = 0;
    bool inputs_valid_ = false, credit_outstanding_ = false, credit_written_ = false, lease_credit_granted_ = false;
#endif
#if NATIVE_TRACE_TCP_INGRESS
    TcpArrivalWindow arrivals_;
#endif
};

}  // namespace native_badge
