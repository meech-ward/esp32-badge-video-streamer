#include "stream_client.hpp"

#include <algorithm>
#include <cstring>
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/time.h"
#if NATIVE_TRACE_TCP_INGRESS
#include "lwip/prot/tcp.h"
#include "tcp_ingress_hook.h"

namespace {
native_badge::StreamClient* ingress_client = nullptr;
}

extern "C" err_t native_tcp_ingress_hook(tcp_pcb* pcb, const tcp_hdr* header, const pbuf* packet) {
    // Called on core0 by the polling stack. Never call TCP, format output,
    // retain a pbuf, or alter protocol state from this observation point.
    if (ingress_client && pcb && header && packet && packet->tot_len &&
        pcb->state == ESTABLISHED && pcb->remote_port == streamer::kVideoPort)
        ingress_client->observe_tcp_segment(pcb, header->seqno, pcb->rcv_nxt, packet->tot_len);
    return ERR_OK;
}
#endif

namespace native_badge {
namespace {
uint32_t millis() { return static_cast<uint32_t>(time_us_64() / 1000); }
bool reached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }
}

void StreamClient::init(uint8_t* jpeg, uint8_t quality, uint8_t* second_jpeg) {
#if NATIVE_TRACE_TCP_INGRESS
    ingress_client = this;
#endif
    jpeg_ = jpeg;
#if NATIVE_JPEG_PSRAM
    inputs_[0] = jpeg;
    inputs_[1] = second_jpeg;
    const uintptr_t a = reinterpret_cast<uintptr_t>(jpeg), b = reinterpret_cast<uintptr_t>(second_jpeg);
    inputs_valid_ = jpeg && second_jpeg && (a < b ? b - a : a - b) >= jpeg_capacity;
#else
    (void)second_jpeg;
#endif
    streamer::pack_hello(hello_, {streamer::kFsQvga, quality, 1, jpeg_capacity / 1024});
}

void StreamClient::disconnected() {
    pcb_ = nullptr;
    connecting_ = hello_sent_ = ready_ = false;
    output_pending_ = false;
    header_got_ = payload_got_ = 0;
    reading_payload_ = skip_ = false;
    credits_owed_ = 0;
    trace_ = {};
    tx_written_ = tx_acked_ = credit_end_ = previous_payload_us_ = 0;
#if NATIVE_JPEG_PSRAM
    credit_outstanding_ = credit_written_ = lease_credit_granted_ = false;
#endif
#if NATIVE_TRACE_TCP_INGRESS
    arrivals_.reset();
#endif
    retry_at_ = millis() + 250;
    // in_use_ is deliberately retained: core 1 may still be decoding. A new
    // connection cannot overwrite JPEG storage until frame_done() releases it.
}

void StreamClient::close() {
    if (pcb_) {
        tcp_arg(pcb_, nullptr);
        tcp_recv(pcb_, nullptr);
        tcp_err(pcb_, nullptr);
        tcp_sent(pcb_, nullptr);
        tcp_abort(pcb_);
    }
    disconnected();
}

void StreamClient::poll(uint32_t now, bool network_ready) {
#if NATIVE_JPEG_PSRAM
    network_ready = network_ready && inputs_valid_;
#endif
    if (!network_ready) {
        if (pcb_) close();
        return;
    }
    if (pcb_ && !hello_sent_ && now - connect_at_ >= 1000) { ++errors; ++connection_failures; close(); }
    if (hello_sent_ && now - last_rx_ >= 3000) { ++errors; close(); }
    if (!pcb_ && !in_use_ && reached(now, retry_at_)) {
        pcb_ = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb_) { retry_at_ = now + 1000; return; }
        tcp_arg(pcb_, this);
        tcp_recv(pcb_, receive_callback);
        tcp_err(pcb_, error_callback);
#if NATIVE_TRACE_GAPS
        tcp_sent(pcb_, sent_callback);
#endif
        tcp_nagle_disable(pcb_);
        connecting_ = true;
        connect_at_ = now;
        ++connects;
        ip_addr_t address;
        IP_ADDR4(&address, 192, 168, 4, 1);
        if (tcp_connect(pcb_, &address, streamer::kVideoPort, connected_callback) != ERR_OK) {
            ++errors;
            ++connection_failures;
            close();
        }
    }
    transmit_pending();
}

err_t StreamClient::connected_callback(void* arg, tcp_pcb*, err_t err) {
    auto& self = *static_cast<StreamClient*>(arg);
    if (err != ERR_OK) { ++self.errors; ++self.connection_failures; self.close(); return ERR_ABRT; }
    self.connecting_ = false;
    self.last_rx_ = millis();
    self.transmit_pending();
    return ERR_OK;
}

void StreamClient::transmit_pending() {
    if (!pcb_ || connecting_) return;
    bool written = false;
    if (!hello_sent_) {
#if NATIVE_TRACE_GAPS
        if (!trace_.credit_queued_us) trace_credit(0);  // initial HELLO credit
#endif
        if (tcp_write(pcb_, hello_, sizeof hello_, TCP_WRITE_FLAG_COPY) != ERR_OK) return;
#if NATIVE_TRACE_GAPS
        tx_written_ += sizeof hello_;
        credit_end_ = tx_written_;
#endif
        hello_sent_ = true;
#if NATIVE_JPEG_PSRAM
        credit_outstanding_ = credit_written_ = true;  // one initial application credit
#endif
        ++session_;
        last_rx_ = millis();
        written = true;
    }
    while (credits_owed_) {
        if (tcp_write(pcb_, &streamer::kCredit, 1, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
#if NATIVE_TRACE_GAPS
        credit_end_ = ++tx_written_;
#endif
        --credits_owed_;
#if NATIVE_JPEG_PSRAM
        credit_written_ = true;
#endif
        written = true;
    }
    if (written) output_pending_ = true;
    // A transient allocation/driver-full error must retry on the next core-0
    // poll, not wait for TCP's much slower retransmission timer.
    if (output_pending_ && tcp_output(pcb_) == ERR_OK) {
        output_pending_ = false;
#if NATIVE_TRACE_GAPS
        if (credit_end_ && !trace_.credit_output_us) trace_.credit_output_us = time_us_32();
#endif
    }
}

void StreamClient::trace_credit(uint16_t source_sequence) {
#if NATIVE_TRACE_GAPS
    trace_.credit_source_seq = source_sequence;
    trace_.credit_queued_us = time_us_32();
    trace_.credit_output_us = trace_.credit_ack_us = 0;
    credit_end_ = 0;
#if NATIVE_TRACE_TCP_INGRESS
    arrivals_.reset();
#endif
#else
    (void)source_sequence;
#endif
}

#if NATIVE_TRACE_TCP_INGRESS
void StreamClient::observe_tcp_segment(const tcp_pcb* pcb, uint32_t sequence, uint32_t expected, uint16_t length) {
    // Exclude unrelated PCBs and payload arriving before the preceding credit
    // has been handed to TCP output. The interval can still include STATUS or
    // duplicates, so first_raw_us is not a camera-frame/radio arrival time.
    if (pcb == pcb_ && trace_.credit_output_us)
        arrivals_.observe(sequence, expected, length, time_us_32());
}
#endif

err_t StreamClient::sent_callback(void* arg, tcp_pcb*, uint16_t length) {
    auto& self = *static_cast<StreamClient*>(arg);
    self.tx_acked_ += length;
    if (self.credit_end_ && !self.trace_.credit_ack_us &&
        static_cast<int32_t>(self.tx_acked_ - self.credit_end_) >= 0) self.trace_.credit_ack_us = time_us_32();
    return ERR_OK;
}

void StreamClient::error_callback(void* arg, err_t) {
    auto& self = *static_cast<StreamClient*>(arg);
    ++self.errors;
    if (!self.hello_sent_) ++self.connection_failures;
    self.disconnected();  // lwIP has already freed the PCB
}

err_t StreamClient::receive_callback(void* arg, tcp_pcb* pcb, pbuf* packet, err_t err) {
    auto& self = *static_cast<StreamClient*>(arg);
    if (!packet || err != ERR_OK) {
        if (packet) pbuf_free(packet);
        ++self.errors;
        self.close();
        return ERR_ABRT;
    }
    self.last_rx_ = millis();
    // TCP's advertised receive window bounds each callback's input. Copy and
    // release it now; never hold network pbufs across JPEG decoding or DMA.
    for (pbuf* part = packet; part; part = part->next) {
        if (!self.consume(static_cast<const uint8_t*>(part->payload), part->len)) {
            pbuf_free(packet);
            ++self.errors;
            self.close();
            return ERR_ABRT;
        }
    }
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
    self.transmit_pending();
    return ERR_OK;
}

bool StreamClient::header_complete() {
    if (!streamer::unpack_header(header_, sizeof header_, meta_)) return false;
#if NATIVE_JPEG_PSRAM
    if (meta_.type == streamer::kTypeFrame) {
        if (ready_ || !credit_outstanding_ || !credit_written_) return false;
        credit_outstanding_ = credit_written_ = false;
        rx_slot_ = in_use_ ? 1u - leased_slot_ : 0;
        jpeg_ = inputs_[rx_slot_];
    }
#else
    if (meta_.type == streamer::kTypeFrame && (ready_ || in_use_)) return false;
#endif
    if (meta_.type == streamer::kTypeFrame && meta_.width != 320) return false;
    skip_ = meta_.type == streamer::kTypeFrame && meta_.len > jpeg_capacity;
    payload_got_ = 0;
    reading_payload_ = true;
    return true;
}

bool StreamClient::consume(const uint8_t* bytes, size_t len) {
    while (len) {
        if (!reading_payload_) {
            if (!header_got_) header_at_us_ = time_us_32();
            const size_t count = std::min(len, sizeof header_ - header_got_);
            std::memcpy(header_ + header_got_, bytes, count);
            header_got_ += count;
            bytes += count;
            len -= count;
            if (header_got_ != sizeof header_) continue;
            if (!header_complete()) return false;
            if (!meta_.len) payload_complete();
        } else {
            const size_t count = std::min(len, static_cast<size_t>(meta_.len) - payload_got_);
            if (!skip_) {
                uint8_t* target = meta_.type == streamer::kTypeFrame ? jpeg_ : status_;
                std::memcpy(target + payload_got_, bytes, count);
            }
            payload_got_ += count;
            bytes += count;
            len -= count;
            if (payload_got_ == meta_.len) payload_complete();
        }
    }
    return true;
}

void StreamClient::payload_complete() {
    reading_payload_ = false;
    header_got_ = 0;
    if (meta_.type == streamer::kTypeStatus) {
        ++statuses;
        last_status_us = time_us_32();
    } else if (skip_) {
        ++oversized;
        ++credits_owed_;  // replace an oversized frame without touching input storage
#if NATIVE_JPEG_PSRAM
        credit_outstanding_ = true;  // replace the consumed oversize credit
        credit_written_ = false;
#endif
        trace_credit(meta_.seq);
    } else {
        const uint32_t received = time_us_32();
        frame_ = {};
        frame_.jpeg = jpeg_;
        frame_.length = meta_.len;
        frame_.sequence = meta_.seq;
        frame_.session = session_;
        frame_.age_us = meta_.age_us;
        frame_.receive_us = received - header_at_us_;
        frame_.received_at_us = received;
#if NATIVE_TRACE_GAPS
        trace_.header_us = header_at_us_;
        trace_.payload_us = received;
        trace_.payload_gap_us = previous_payload_us_ ? received - previous_payload_us_ : 0;
        previous_payload_us_ = received;
#if NATIVE_TRACE_TCP_INGRESS
        trace_.ingress = arrivals_.snapshot(received);
#endif
        frame_.trace = trace_;
#endif
        last_frame_bytes = meta_.len;
        max_frame_bytes = std::max(max_frame_bytes, meta_.len);
        ++frames;
        ready_ = true;
    }
}

bool StreamClient::take_frame(ReceivedFrame& frame) {
    if (!ready_ || in_use_) return false;
    frame = frame_;
    if (++lease_id_ == 0) ++lease_id_;
    frame.lease = lease_id_;
    leased_frame_ = frame;
#if NATIVE_JPEG_PSRAM
    leased_slot_ = rx_slot_;
    lease_credit_granted_ = false;
#endif
    ready_ = false;
    in_use_ = true;
    return true;
}

void StreamClient::frame_done() {
    (void)frame_done(leased_frame_);
}

bool StreamClient::owns_lease(const ReceivedFrame& frame) const {
    return in_use_ && frame.lease && frame.lease == leased_frame_.lease &&
        frame.session == leased_frame_.session && frame.jpeg == leased_frame_.jpeg;
}

bool StreamClient::prefetch_credit(const ReceivedFrame& lease) {
#if NATIVE_JPEG_PSRAM
    if (!owns_lease(lease) || !hello_sent_ || lease_credit_granted_ || credit_outstanding_ || ready_ ||
        (reading_payload_ && meta_.type == streamer::kTypeFrame)) return false;
    lease_credit_granted_ = credit_outstanding_ = true;
    credit_written_ = false;
    ++credits_owed_;
    ++early_credits;
    trace_credit(lease.sequence);
    transmit_pending();
    return true;
#else
    (void)lease;
    return false;
#endif
}

bool StreamClient::frame_done(const ReceivedFrame& lease) {
    if (!owns_lease(lease)) return false;
    in_use_ = false;
    if (hello_sent_) {
#if NATIVE_JPEG_PSRAM
        if (lease_credit_granted_) return true;  // never duplicate an early credit
        credit_outstanding_ = true;
        credit_written_ = false;
#endif
        ++credits_owed_;
        ++late_credits;
        trace_credit(lease.sequence);
        transmit_pending();
    }
    return true;
}

}  // namespace native_badge
