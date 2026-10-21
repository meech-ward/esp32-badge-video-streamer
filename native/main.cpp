// Native Tufty 2350 receiver: core 0 owns radio/input/LCD, core 1 only JPEG.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <atomic>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdio_usb.h"
#include "pico/util/queue.h"
#include "hardware/watchdog.h"
#include "hud_button.hpp"
#include "display.hpp"
#include "jpeg_decode.hpp"
#include "stream_client.hpp"
#include "link_policy.hpp"
#include "frame_freshness.hpp"
#include "hud.hpp"
#if NATIVE_JPEG_PSRAM
#include "jpeg_pipeline.hpp"
#include "psram.hpp"
#endif
#include "tusb.h"
#include "secrets.h"  // ignored local input; see secrets.example.h

#ifndef NATIVE_JPEG_QUALITY
#define NATIVE_JPEG_QUALITY 6
#endif
#ifndef NATIVE_DIAGNOSTICS
#define NATIVE_DIAGNOSTICS 0
#endif
#ifndef NATIVE_CYW43_BOUNDED_TX
#define NATIVE_CYW43_BOUNDED_TX 0
#endif
#ifndef NATIVE_JPEG_SRAM
#define NATIVE_JPEG_SRAM 0
#endif
#ifndef NATIVE_PREFETCH_LEAD_US
#define NATIVE_PREFETCH_LEAD_US 0
#endif
static_assert(NATIVE_JPEG_QUALITY >= 4 && NATIVE_JPEG_QUALITY <= 63);
static_assert(sizeof(NATIVE_WIFI_PASSWORD) > 8 && sizeof(NATIVE_WIFI_PASSWORD) <= 64);

namespace {
using namespace native_badge;

// Two RGB images stay in SRAM. PSRAM holds only a leased compressed input
// and one incoming input; the fallback keeps a single compressed SRAM input.
alignas(4) uint16_t rgb[2][native_badge::pixels];
#if NATIVE_JPEG_PSRAM
GuardedJpegInputs jpeg_inputs{reinterpret_cast<uint8_t*>(psram::uncached_base)};
static_assert(GuardedJpegInputs::capacity == StreamClient::jpeg_capacity);
std::atomic<uint32_t> decoder_started_us{0};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
#else
alignas(4) uint8_t jpeg[StreamClient::jpeg_capacity];
#endif
alignas(8) uint32_t decoder_stack[2048];

enum class Ownership : uint8_t { Free, Decoding, Ready, Displaying };
Ownership owner[2] = {Ownership::Free, Ownership::Free};
struct DecodeJob { ReceivedFrame frame; unsigned slot; };
struct DecodeCompletion {
    unsigned slot;
    bool ok;
    uint32_t elapsed_us;
#if NATIVE_DIAGNOSTICS
    uint32_t started_us, finished_us;
#endif
};
struct SlotTiming {
    ReceivedFrame frame;
#if NATIVE_DIAGNOSTICS
    uint32_t decode_started = 0, decode_finished = 0, present_started = 0;
#endif
};
SlotTiming slot_timing[2];
queue_t jobs, completions;

void decoder_core() {
    for (;;) {
        DecodeJob job;
        queue_remove_blocking(&jobs, &job);
#if NATIVE_JPEG_PSRAM
        __dmb();
#endif
        const uint32_t start = time_us_32();
#if NATIVE_JPEG_PSRAM
        decoder_started_us.store(start, std::memory_order_release);
#endif
        const DecodeResult result = decode_jpeg_rgb565(job.frame.jpeg, job.frame.length,
            rgb[job.slot], native_badge::pixels, 320, 240);
        const uint32_t finished = time_us_32();
#if NATIVE_JPEG_PSRAM
        decoder_started_us.store(0, std::memory_order_release);
#endif
        const DecodeCompletion done{job.slot, result.ok, finished - start
#if NATIVE_DIAGNOSTICS
            , start, finished
#endif
        };
        // One job at a time. A completed decoder never touches pixels again
        // until a new job explicitly returns ownership to this core.
        queue_add_blocking(&completions, &done);
    }
}

#if NATIVE_DIAGNOSTICS
constexpr uint32_t bounds[] = {1000,2000,4000,6000,8000,10000,12000,14000,16000,18000,
    20000,22000,24000,26000,28000,30000,32000,34000,36000,38000,40000,42000,44000,46000,
    48000,50000,55000,60000,67000,75000,85000,100000,125000,150000,200000,250000,
    500000,1000000,UINT32_MAX};
struct Distribution {
    uint32_t count = 0, maximum = 0;
    uint64_t sum = 0;
    uint32_t bins[std::size(bounds)]{};
    void add(uint32_t us) {
        ++count;
        sum += us;
        maximum = std::max(maximum, us);
        size_t bucket = 0;
        while (us > bounds[bucket]) ++bucket;
        ++bins[bucket];
    }
    uint32_t mean() const { return count ? static_cast<uint32_t>(sum / count) : 0; }
    uint32_t p95() const {
        if (!count) return 0;
        const uint32_t wanted = (count * 95 + 99) / 100;
        uint32_t seen = 0;
        for (size_t i = 0; i < std::size(bounds); ++i) {
            seen += bins[i];
            if (seen >= wanted) return bounds[i] == UINT32_MAX ? maximum : bounds[i];
        }
        return maximum;
    }
};

struct Window {
    Distribution decode, dma, gap;
    Distribution esp_age, receive, queue_delay, ready_start, ready_complete, receive_complete;
    uint32_t presented = 0, gap50 = 0, gap100 = 0, gap250 = 0, loop_max = 0;
} window;
#else
// Keep the HUD and a compact health record without per-event histogram work.
struct Window {
    uint64_t decode_sum = 0;
    uint32_t decoded = 0, presented = 0, gap_max = 0;
    uint32_t decode_mean() const {
        return decoded ? static_cast<uint32_t>(decode_sum / decoded) : 0;
    }
} window;
#endif
struct Totals {
    uint32_t presented = 0, decode_errors = 0, recoveries = 0, stale_frames = 0;
#if NATIVE_DIAGNOSTICS
    uint32_t gap50 = 0, gap100 = 0, gap250 = 0, recovery_max_us = 0;
#endif
} totals;
#if NATIVE_DIAGNOSTICS
struct PowerState {
    int set_result = -999, get_result = -999;
    uint32_t value = UINT32_MAX, reads = 0, elapsed_us = 0;
} power_state;
char pending_stages[896];
size_t pending_stage_length = 0;
#endif
#if NATIVE_JPEG_PSRAM
uint32_t guard_errors = 0;
#if NATIVE_DIAGNOSTICS
char pending_pipeline[512];
size_t pending_pipeline_length = 0;
struct PipelineWindow {
    uint64_t prediction_sum = 0, lead_sum = 0;
    uint32_t predictions = 0, prediction_max = 0, lead_samples = 0, lead_max = 0, after_done = 0;
} pipeline_window;
#endif
#endif
#if NATIVE_TRACE_GAPS
char pending_trace[512];
size_t pending_trace_length = 0;
#endif

uint32_t millis() { return static_cast<uint32_t>(time_us_64() / 1000); }
bool reached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

uint32_t known_frame_age_us(const ReceivedFrame& frame, uint32_t now_us) {
    // Sum durations, never unlike clocks: ESP capture→send start, local first
    // ordered header→payload end, and local time since payload completion.
    // This excludes network transit before the first ordered header byte and
    // therefore cannot claim a complete capture-to-display measurement.
    const uint64_t age = static_cast<uint64_t>(frame.age_us) + frame.receive_us +
        static_cast<uint32_t>(now_us - frame.received_at_us);
    return static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, age));
}

bool usb_line(const char* line, size_t length) {
    // One bounded record, never a partial JSON line. No waiting for a slow
    // host; diagnostic callers retain at most their newest pending record.
    if (!stdio_usb_connected() || tud_cdc_write_available() < length + 2) return false;
    std::printf("%s\n", line);
    return true;
}

#if NATIVE_DIAGNOSTICS
void flush_stages() {
    if (pending_stage_length && usb_line(pending_stages, pending_stage_length)) pending_stage_length = 0;
#if NATIVE_TRACE_GAPS
    if (pending_trace_length && usb_line(pending_trace, pending_trace_length)) pending_trace_length = 0;
#endif
#if NATIVE_JPEG_PSRAM
    if (pending_pipeline_length && usb_line(pending_pipeline, pending_pipeline_length)) pending_pipeline_length = 0;
#endif
}
#endif

void trace_frame(const ReceivedFrame& frame) {
#if NATIVE_TRACE_GAPS
    if (frame.trace.payload_gap_us < 75000) return;
    const FrameTrace& t = frame.trace;
    const int n = std::snprintf(pending_trace, sizeof pending_trace,
        "{\"native_trace\":1,\"session\":%lu,\"seq\":%u,\"credit_source_seq\":%u,\"credit_queued_us\":%lu,"
        "\"credit_output_us\":%lu,\"credit_ack_us\":%lu,\"header_us\":%lu,\"payload_us\":%lu,\"payload_gap_us\":%lu"
#if NATIVE_TRACE_TCP_INGRESS
        ",\"first_raw_us\":%lu,\"raw_segments\":%lu,\"ahead_segments\":%lu,\"behind_segments\":%lu,"
        "\"max_ahead_bytes\":%lu,\"cover_events\":%lu,\"cover_wait_max_us\":%lu,\"pending_ahead_us\":%lu"
#endif
        "}",
        static_cast<unsigned long>(frame.session), static_cast<unsigned>(frame.sequence), static_cast<unsigned>(t.credit_source_seq),
        static_cast<unsigned long>(t.credit_queued_us), static_cast<unsigned long>(t.credit_output_us),
        static_cast<unsigned long>(t.credit_ack_us), static_cast<unsigned long>(t.header_us),
        static_cast<unsigned long>(t.payload_us), static_cast<unsigned long>(t.payload_gap_us)
#if NATIVE_TRACE_TCP_INGRESS
        , static_cast<unsigned long>(t.ingress.first_raw_us), static_cast<unsigned long>(t.ingress.segments),
        static_cast<unsigned long>(t.ingress.ahead), static_cast<unsigned long>(t.ingress.behind),
        static_cast<unsigned long>(t.ingress.max_ahead_bytes), static_cast<unsigned long>(t.ingress.cover_events),
        static_cast<unsigned long>(t.ingress.cover_wait_max_us), static_cast<unsigned long>(t.ingress.pending_ahead_us)
#endif
        );
    pending_trace_length = n > 0 && n < static_cast<int>(sizeof pending_trace) ? static_cast<size_t>(n) : 0;
#else
    (void)frame;
#endif
}

void report(const StreamClient& stream, uint32_t elapsed_us,
            int wifi_status, uint32_t sample, bool video_fresh) {
    // No formatting cost when disconnected. USB never paces the video loop.
    if (!stdio_usb_connected()) return;
    const uint32_t fps100 = elapsed_us
        ? static_cast<uint32_t>(static_cast<uint64_t>(window.presented) * 100000000 / elapsed_us) : 0;
    char line[640];
    const int length = std::snprintf(line, sizeof line,
        "{\"native\":1,\"sample\":%lu,\"diagnostics\":%s,\"w\":320,\"h\":240,\"q\":%u,"
        "\"fps100\":%lu,\"decode_us\":%lu,\"gap_max_us\":%lu,\"presented\":%lu,\"received\":%lu,"
        "\"decode_errors\":%lu,\"stream_errors\":%lu,\"oversize\":%lu,\"connects\":%lu,"
        "\"wifi_recoveries\":%lu,\"stale_frames\":%lu,\"guard_errors\":%lu,\"video_fresh\":%s}",
        static_cast<unsigned long>(sample), NATIVE_DIAGNOSTICS ? "true" : "false",
        static_cast<unsigned>(NATIVE_JPEG_QUALITY), static_cast<unsigned long>(fps100),
#if NATIVE_DIAGNOSTICS
        static_cast<unsigned long>(window.decode.mean()), static_cast<unsigned long>(window.gap.maximum),
#else
        static_cast<unsigned long>(window.decode_mean()), static_cast<unsigned long>(window.gap_max),
#endif
        static_cast<unsigned long>(totals.presented), static_cast<unsigned long>(stream.frames),
        static_cast<unsigned long>(totals.decode_errors), static_cast<unsigned long>(stream.errors),
        static_cast<unsigned long>(stream.oversized), static_cast<unsigned long>(stream.connects),
        static_cast<unsigned long>(totals.recoveries), static_cast<unsigned long>(totals.stale_frames),
#if NATIVE_JPEG_PSRAM
        static_cast<unsigned long>(guard_errors),
#else
        0ul,
#endif
        video_fresh ? "true" : "false");
    if (length > 0 && length < static_cast<int>(sizeof line)) usb_line(line, static_cast<size_t>(length));
#if NATIVE_DIAGNOSTICS
    const int stage_length = std::snprintf(pending_stages, sizeof pending_stages,
        "{\"native_stages\":1,\"sample\":%lu,\"wifi\":%d,\"n\":%lu,"
        "\"decode_p95_us\":%lu,\"decode_max_us\":%lu,\"dma_us\":%lu,\"gap_p95_us\":%lu,"
        "\"gap_gt50\":%lu,\"gap_gt100\":%lu,\"gap_gt250\":%lu,\"loop_max_us\":%lu,"
        "\"esp_age_us\":%lu,\"receive_us\":%lu,\"queue_us\":%lu,\"ready_start_us\":%lu,"
        "\"ready_complete_us\":%lu,\"receive_complete_us\":%lu,\"recovery_max_us\":%lu,"
        "\"pm_set\":%d,\"pm_get\":%d,\"pm\":%lu}",
        static_cast<unsigned long>(sample), wifi_status, static_cast<unsigned long>(window.presented),
        static_cast<unsigned long>(window.decode.p95()), static_cast<unsigned long>(window.decode.maximum),
        static_cast<unsigned long>(window.dma.mean()), static_cast<unsigned long>(window.gap.p95()),
        static_cast<unsigned long>(window.gap50), static_cast<unsigned long>(window.gap100),
        static_cast<unsigned long>(window.gap250), static_cast<unsigned long>(window.loop_max),
        static_cast<unsigned long>(window.esp_age.mean()), static_cast<unsigned long>(window.receive.mean()),
        static_cast<unsigned long>(window.queue_delay.mean()), static_cast<unsigned long>(window.ready_start.mean()),
        static_cast<unsigned long>(window.ready_complete.mean()), static_cast<unsigned long>(window.receive_complete.mean()),
        static_cast<unsigned long>(totals.recovery_max_us), power_state.set_result, power_state.get_result,
        static_cast<unsigned long>(power_state.value));
    pending_stage_length = stage_length > 0 && stage_length < static_cast<int>(sizeof pending_stages)
        ? static_cast<size_t>(stage_length) : 0;
#if NATIVE_JPEG_PSRAM
    const int pipeline_length = std::snprintf(pending_pipeline, sizeof pending_pipeline,
        "{\"native_pipeline\":1,\"sample\":%lu,\"lead_us\":%u,\"early_credits\":%lu,\"late_credits\":%lu,"
        "\"prediction_abs_us\":%lu,\"prediction_max_us\":%lu,\"credit_lead_us\":%lu,\"credit_lead_max_us\":%lu,"
        "\"after_done_grants\":%lu,\"guard_errors\":%lu}",
        static_cast<unsigned long>(sample), static_cast<unsigned>(NATIVE_PREFETCH_LEAD_US),
        static_cast<unsigned long>(stream.early_credits), static_cast<unsigned long>(stream.late_credits),
        static_cast<unsigned long>(pipeline_window.predictions ? pipeline_window.prediction_sum / pipeline_window.predictions : 0),
        static_cast<unsigned long>(pipeline_window.prediction_max),
        static_cast<unsigned long>(pipeline_window.lead_samples ? pipeline_window.lead_sum / pipeline_window.lead_samples : 0),
        static_cast<unsigned long>(pipeline_window.lead_max), static_cast<unsigned long>(pipeline_window.after_done),
        static_cast<unsigned long>(guard_errors));
    pending_pipeline_length = pipeline_length > 0 && pipeline_length < static_cast<int>(sizeof pending_pipeline)
        ? static_cast<size_t>(pipeline_length) : 0;
#endif
#else
    (void)wifi_status;
#endif
}

[[noreturn]] void leave_to_bootsel(StreamClient& stream) {
    stream.close();
    reset_usb_boot(0, 0);
    for (;;) tight_loop_contents();
}

}  // namespace

int main() {
    stdio_init_all();
    native_badge::init();
#if NATIVE_JPEG_PSRAM
    // Recover a bad external-memory bus configuration into BOOTSEL instead
    // of repeatedly touching PSRAM after a watchdog fault. No radio/core1 yet.
    if (native_badge::home_pressed() || watchdog_enable_caused_reboot()) reset_usb_boot(0, 0);
    watchdog_enable(4000, true);
    const auto memory = psram::init_boot_only();
    if (memory.bytes < GuardedJpegInputs::bytes) reset_usb_boot(0, 0);
    jpeg_inputs.initialize_guards();
    if (!jpeg_inputs.intact()) reset_usb_boot(0, 0);
    watchdog_update();
#endif
    HudState hud;
    draw_waiting_screen(rgb[0], hud);
    int displaying = native_badge::present(rgb[0]) ? 0 : -1;
    if (displaying == 0) owner[0] = Ownership::Displaying;
    bool displaying_video = false;
    bool waiting_screen_shown = true;
    queue_init(&jobs, sizeof(DecodeJob), 1);
    queue_init(&completions, sizeof(DecodeCompletion), 1);
    multicore_launch_core1_with_stack(decoder_core, decoder_stack, sizeof decoder_stack);
    const bool radio_ok = cyw43_arch_init() == 0;
    if (radio_ok) {
        cyw43_arch_enable_sta_mode();
        cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    }
    StreamClient stream;
#if NATIVE_JPEG_PSRAM
    stream.init(jpeg_inputs.slot(0), NATIVE_JPEG_QUALITY, jpeg_inputs.slot(1));
#else
    stream.init(jpeg, NATIVE_JPEG_QUALITY);
#endif
    HudButton hud_button;
    watchdog_enable(4000, true);
#if NATIVE_JPEG_PSRAM
    DecodeForecast forecast;
    bool pipeline_failed = false, job_prefetched = false;
    unsigned decoding_slot = 0;
    uint32_t checked_received_frames = 0;
#if NATIVE_DIAGNOSTICS
    uint32_t predicted_duration = 0, prefetch_queued_at = 0;
#endif
    uint32_t forecast_session = 0;
    const auto check_inputs = [&]() {
        if (!pipeline_failed && !jpeg_inputs.intact()) {
            pipeline_failed = true;
            ++guard_errors;
            stream.close();
        }
        return !pipeline_failed;
    };
#endif

    bool decoding = false, was_up = false, joining = false, recovering = false;
    LinkRecovery recovery_policy;
    FrameFreshness video_freshness;
    uint32_t recovery_resume = 0;
    uint32_t join_at = 0, next_join = 0, next_input = 0;
    uint32_t last_present = 0, report_sample = 0;
    uint32_t hud_fps100 = 0, last_decode_us = 0, last_gap_us = 0;
    uint32_t report_at = time_us_32();
#if NATIVE_DIAGNOSTICS
    uint32_t last_loop = report_at;
#endif
    for (;;) {
#if NATIVE_DIAGNOSTICS
        const uint32_t now_us = time_us_32();
        window.loop_max = std::max(window.loop_max, now_us - last_loop);
        last_loop = now_us;
#endif
        watchdog_update();
        if (radio_ok) cyw43_arch_poll();
        uint32_t now = millis();
        const int link = radio_ok ? cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) : CYW43_LINK_FAIL;
        bool up = link == CYW43_LINK_UP && !recovering;
        if (recovering && reached(now, recovery_resume)) {
#if NATIVE_DIAGNOSTICS
            const uint32_t start = time_us_32();
#endif
            cyw43_arch_enable_sta_mode();
            watchdog_update();
            cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
#if NATIVE_DIAGNOSTICS
            totals.recovery_max_us = std::max(totals.recovery_max_us, time_us_32() - start);
#endif
            recovering = false;
            joining = false;
            next_join = millis();
            up = false;  // wait for a fresh async join/IP status on the next loop
        }
        if (radio_ok && !recovering && !up && reached(now, next_join) && (!joining || now - join_at >= 15000)) {
            joining = cyw43_arch_wifi_connect_async(NATIVE_WIFI_SSID, NATIVE_WIFI_PASSWORD,
                CYW43_AUTH_WPA2_AES_PSK) == 0;
            join_at = now;
            next_join = now + 1000;
        }
        if (link < 0) joining = false;
        if (up && !was_up) {
            joining = false;
#if NATIVE_DIAGNOSTICS
            const uint32_t start = time_us_32();
            power_state.set_result = cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
            watchdog_update();
            power_state.value = UINT32_MAX;
            power_state.get_result = cyw43_wifi_get_pm(&cyw43_state, &power_state.value);
            power_state.elapsed_us = time_us_32() - start;
            ++power_state.reads;  // one readback per association, never per report
#else
            cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
            watchdog_update();
#endif
            now = millis();
        }
        was_up = up;
        stream.poll(now, up
#if NATIVE_JPEG_PSRAM
            && !pipeline_failed
#endif
        );
#if NATIVE_JPEG_PSRAM
        if (forecast_session != stream.session()) {
            forecast_session = stream.session();
            forecast.reset();  // first frame of each TCP session is late-credit
        }
        if (stream.frame_ready() && stream.frames != checked_received_frames) {
            checked_received_frames = stream.frames;
            check_inputs();
        }
#endif
        if (recovery_policy.poll(now, up
#if NATIVE_JPEG_PSRAM
            && !pipeline_failed
#endif
            , stream.frames, stream.connection_failures)) {
            // Association/IP can remain cached UP after an ESP reset. Drop the
            // TCP session and netif, then asynchronously join again. Core1's
            // JPEG lease survives close(); the input is never reused early.
#if NATIVE_DIAGNOSTICS
            const uint32_t start = time_us_32();
#endif
            stream.close();
            watchdog_update();
            cyw43_arch_disable_sta_mode();
            watchdog_update();
#if NATIVE_DIAGNOSTICS
            totals.recovery_max_us = std::max(totals.recovery_max_us, time_us_32() - start);
#endif
            ++totals.recoveries;
            recovery_policy.reset();
            recovering = true;
            recovery_resume = millis() + 250;
            joining = was_up = up = false;
            now = millis();
        }

        now = millis();
        if (!stream.connected()) video_freshness.clear();
        if (reached(now, next_input)) {
            next_input = now + 1;
            // Only B is used. Debounce independently of frame arrival so a
            // stalled camera cannot delay inputs or cause repeated toggles.
            if (hud_button.update(now, (native_badge::buttons() & 2u) != 0)) {
                hud.stats_visible = !hud.stats_visible;
                waiting_screen_shown = false;
            }
            if (native_badge::home_pressed()) leave_to_bootsel(stream);
        }

        const bool lcd_busy = native_badge::busy();
        if (displaying >= 0 && !lcd_busy) {
            const unsigned slot = static_cast<unsigned>(displaying);
            owner[slot] = Ownership::Free;
            displaying = -1;
            const uint32_t finished = time_us_32();
            if (displaying_video) {
                const SlotTiming& timing = slot_timing[slot];
#if NATIVE_DIAGNOSTICS
                window.esp_age.add(timing.frame.age_us);
                window.receive.add(timing.frame.receive_us);
                window.queue_delay.add(timing.decode_started - timing.frame.received_at_us);
                window.ready_start.add(timing.present_started - timing.decode_finished);
                window.ready_complete.add(finished - timing.decode_finished);
                window.receive_complete.add(finished - timing.frame.received_at_us);
                window.dma.add(native_badge::last_transfer_us());
#endif
                if (last_present) {
                    const uint32_t gap = finished - last_present;
                    last_gap_us = gap;
#if NATIVE_DIAGNOSTICS
                    window.gap.add(gap);
                    window.gap50 += gap > 50000;
                    window.gap100 += gap > 100000;
                    window.gap250 += gap > 250000;
                    totals.gap50 += gap > 50000;
                    totals.gap100 += gap > 100000;
                    totals.gap250 += gap > 250000;
#else
                    window.gap_max = std::max(window.gap_max, gap);
#endif
                }
                last_present = finished;
                ++window.presented;
                ++totals.presented;
                const uint32_t known_age = known_frame_age_us(timing.frame, finished);
                if (stream.connected() && timing.frame.session == stream.session() && known_age < 300000)
                    video_freshness.presented(millis(), known_age / 1000);
                else video_freshness.clear();
            }
        }
        DecodeCompletion done;
        if (queue_try_remove(&completions, &done)) {
            decoding = false;
#if NATIVE_DIAGNOSTICS
            window.decode.add(done.elapsed_us);
#else
            window.decode_sum += done.elapsed_us;
            ++window.decoded;
#endif
            last_decode_us = done.elapsed_us;
#if NATIVE_JPEG_PSRAM
            check_inputs();
            const bool valid_decode = done.ok && !pipeline_failed && stream.connected() &&
                slot_timing[done.slot].frame.session == stream.session();
#if NATIVE_DIAGNOSTICS
            if (valid_decode && predicted_duration) {
                const uint32_t error = done.elapsed_us > predicted_duration ? done.elapsed_us - predicted_duration : predicted_duration - done.elapsed_us;
                pipeline_window.prediction_sum += error;
                ++pipeline_window.predictions;
                pipeline_window.prediction_max = std::max(pipeline_window.prediction_max, error);
            }
#endif
            forecast.completed(done.elapsed_us, valid_decode);
#if NATIVE_DIAGNOSTICS
            if (job_prefetched) {
                const int32_t lead = static_cast<int32_t>(done.finished_us - prefetch_queued_at);
                if (lead >= 0) {
                    pipeline_window.lead_sum += static_cast<uint32_t>(lead);
                    ++pipeline_window.lead_samples;
                    pipeline_window.lead_max = std::max(pipeline_window.lead_max, static_cast<uint32_t>(lead));
                } else ++pipeline_window.after_done;
            }
#endif
#endif
            owner[done.slot] = done.ok && stream.connected() &&
                slot_timing[done.slot].frame.session == stream.session() ? Ownership::Ready : Ownership::Free;
            if (!done.ok) ++totals.decode_errors;
            else if (owner[done.slot] == Ownership::Free) ++totals.stale_frames;
#if NATIVE_DIAGNOSTICS
            slot_timing[done.slot].decode_started = done.started_us;
            slot_timing[done.slot].decode_finished = done.finished_us;
#endif
            stream.frame_done(slot_timing[done.slot].frame);
        }
#if NATIVE_JPEG_PSRAM
        if (decoding && !job_prefetched && !pipeline_failed && stream.connected()) {
            const uint32_t started = decoder_started_us.load(std::memory_order_acquire);
            const uint32_t current = time_us_32();  // after publication and any driver work
            if (forecast.due(current, started, NATIVE_PREFETCH_LEAD_US) && check_inputs()) {
                // The other input is independent. The client refuses a duplicate
                // credit and retains this job's lease through close/reconnect.
#if NATIVE_DIAGNOSTICS
                const uint32_t queued_at = time_us_32();
#endif
                if (stream.prefetch_credit(slot_timing[decoding_slot].frame)) {
                    job_prefetched = true;
#if NATIVE_DIAGNOSTICS
                    prefetch_queued_at = queued_at;
#endif
                }
            }
        }
#endif

        const bool video_fresh = video_freshness.fresh(millis());
        hud.fps100 = hud_fps100;
        hud.decode_us = last_decode_us;
        hud.gap_us = last_gap_us;
        hud.connected = stream.connected();

        if (displaying < 0) {
            for (unsigned i = 0; i < 2; ++i) {
                if (owner[i] != Ownership::Ready) continue;
                if (!stream.connected() || slot_timing[i].frame.session != stream.session() ||
                        known_frame_age_us(slot_timing[i].frame, time_us_32()) >= 300000) {
                    owner[i] = Ownership::Free;
                    ++totals.stale_frames;
                    continue;  // freshness wins over displaying a queued old image
                }
                draw_hud(rgb[i], hud);  // core1 and LCD both released this slot
#if NATIVE_DIAGNOSTICS
                const uint32_t start = time_us_32();
#endif
                if (native_badge::present(rgb[i])) {
#if NATIVE_DIAGNOSTICS
                    slot_timing[i].present_started = start;
#endif
                    owner[i] = Ownership::Displaying;
                    displaying = static_cast<int>(i);
                    displaying_video = true;
                    waiting_screen_shown = false;
                }
                break;
            }
            if (displaying < 0 && !stream.connected() && !waiting_screen_shown) {
                for (unsigned i = 0; i < 2; ++i) {
                    if (owner[i] != Ownership::Free) continue;
                    draw_waiting_screen(rgb[i], hud);
                    if (native_badge::present(rgb[i])) {
                        owner[i] = Ownership::Displaying;
                        displaying = static_cast<int>(i);
                        displaying_video = false;
                        waiting_screen_shown = true;
                    }
                    break;
                }
            }
        }
        if (!decoding && stream.frame_ready()) {
            for (unsigned i = 0; i < 2; ++i) {
                if (owner[i] != Ownership::Free) continue;
                ReceivedFrame frame;
                if (stream.take_frame(frame)) {
                    trace_frame(frame);
                    if (frame.session != stream.session() || known_frame_age_us(frame, time_us_32()) >= 300000) {
                        ++totals.stale_frames;
                        stream.frame_done(frame);  // no decoder owns an already-stale JPEG
                        break;
                    }
                    owner[i] = Ownership::Decoding;
                    slot_timing[i].frame = frame;
                    const DecodeJob job{frame, i};
#if NATIVE_JPEG_PSRAM
                    decoding_slot = i;
#if NATIVE_DIAGNOSTICS
                    predicted_duration = forecast.estimate_us();
#endif
                    job_prefetched = false;
                    decoder_started_us.store(0, std::memory_order_release);
                    __dmb();  // uncached PSRAM writes precede the core1 handoff
#endif
                    // Queue capacity is one and decoding is false, so no other
                    // job can exist. A defensive failure releases the JPEG.
                    if (queue_try_add(&jobs, &job)) decoding = true;
                    else { owner[i] = Ownership::Free; stream.frame_done(frame); ++totals.decode_errors; }
                }
                break;
            }
        }
        if (time_us_32() - report_at >= 2000000) {
            const uint32_t end = time_us_32();
            hud_fps100 = static_cast<uint32_t>(static_cast<uint64_t>(window.presented) * 100000000 / (end - report_at));
            report(stream, end - report_at, link, ++report_sample, video_fresh);
            window = {};
#if NATIVE_JPEG_PSRAM && NATIVE_DIAGNOSTICS
            pipeline_window = {};
#endif
            report_at = end;
        }
#if NATIVE_DIAGNOSTICS
        flush_stages();
#endif
        sleep_us(200);
    }
}
