#include "video_tx.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#include "camera_source.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "streamer/protocol.hpp"

namespace video_tx {
namespace {

constexpr const char* TAG = "video";
// Below lwIP (18): sending frames must never starve the network stack.
constexpr UBaseType_t kPrio = 5;

constexpr int64_t kStatusPeriodUs = 1'000'000;
constexpr int64_t kStatsPeriodUs = 2'000'000;
constexpr int64_t kHelloTimeoutUs = 2'000'000;
// A send that makes no progress for this long means the client is gone or stuck.
constexpr int64_t kSendStallUs = 1'500'000;
// A client that holds no credits and sends nothing for this long is dropped,
// returning the camera to the idle (diagnostic) loop.
constexpr int64_t kIdleTimeoutUs = 10'000'000;
// Longest one camera wait may block this task. Short enough that STATUS (1 s),
// accept preemption and the timeouts above stay on time if the sensor stalls;
// a slower frame just means the loop goes round and waits again.
constexpr uint32_t kGrabWaitMs = 100;

// ---------------------------------------------------------------- stats ----

struct Window {
    int64_t start_us = 0;
    uint32_t grabs = 0;
    uint32_t sent = 0;
    uint64_t bytes = 0;
    uint32_t max_len = 0;
    void reset(int64_t now) { *this = Window{}; start_us = now; }
    void on_grab(uint32_t len) {
        ++grabs;
        bytes += len;
        max_len = std::max(max_len, len);
    }
};

struct Report {
    float cam_fps = 0, sent_fps = 0, avg_kb = 0, max_kb = 0;
    bool client = false;
};

// ------------------------------------------------------------ sample ----

// Software AWB requests a copy through a request/done handshake, filled from a
// frame this task grabbed anyway (sent, or the idle loop's), never an extra
// grab, so it costs no credit, no camera frame and no wait in the send path;
// the copy is made after the send. With a client that holds no credit no frame is
// grabbed, so a request simply waits (soft_awb times out and retries).
uint8_t* s_tap_buf = nullptr;  // PSRAM, kMaxPayload bytes
std::atomic<uint32_t> s_tap_req{0};
std::atomic<uint32_t> s_tap_done{0};
uint32_t s_tap_len = 0;
uint16_t s_tap_w = 0, s_tap_h = 0;
int64_t s_tap_us = 0;

bool tap_wanted() { return s_tap_buf && s_tap_req.load() != s_tap_done.load(); }

void tap(const camera_source::Frame& f) {
    const uint32_t req = s_tap_req.load();
    if (!s_tap_buf || req == s_tap_done.load() || f.len == 0 || f.len > streamer::kMaxPayload) return;
    std::memcpy(s_tap_buf, f.data, f.len);
    s_tap_len = f.len;
    s_tap_w = f.width;
    s_tap_h = f.height;
    s_tap_us = f.timestamp_us;
    s_tap_done.store(req);
}

float per_sec(uint32_t n, int64_t dt_us) { return dt_us > 0 ? n * 1e6f / dt_us : 0; }

void format_line(char* out, size_t cap, const Report& r) {
    const uint8_t fs = camera_source::framesize();
    int n = snprintf(out, cap, "cam %.1ffps avg %.1fKB max %.1fKB", r.cam_fps, r.avg_kb, r.max_kb);
    if (r.client) n += snprintf(out + n, cap - n, " | sent %.1ffps", r.sent_fps);
    else n += snprintf(out + n, cap - n, " | no client");
    snprintf(out + n, cap - n,
             " | %s fs%u %ux%u q%u | heap %uKB psram %uKB",
             camera_source::sensor_name(), fs, streamer::kFrameSizes[fs].w, streamer::kFrameSizes[fs].h,
             camera_source::quality(), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// ---------------------------------------------------------- socket I/O ----

enum class Io { Ok, Closed, Preempted };

void close_hard(int fd) {
    // RST instead of FIN: frees the PCB and its 32 KB send buffer (internal
    // RAM) at once rather than retransmitting for minutes to a gone client.
    // Needs CONFIG_LWIP_SO_LINGER (sdkconfig.defaults); without it lwIP
    // rejects the option and close() falls back to a FIN close.
    linger lg = {1, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "SO_LINGER failed (errno %d): dropped clients close with FIN", errno);
        }
    }
    close(fd);
}

bool set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

// Sends every byte of iov[0..n) without blocking the task indefinitely.
// Returns Preempted if a new client is waiting on the listen socket (it
// replaces this one, so the half-sent message does not matter).
Io send_all(int fd, int listen_fd, iovec* iov, int n) {
    int64_t last_progress = esp_timer_get_time();
    while (n > 0) {
        msghdr msg = {};
        msg.msg_iov = iov;
        msg.msg_iovlen = n;
        ssize_t r = sendmsg(fd, &msg, MSG_DONTWAIT);
        if (r > 0) {
            last_progress = esp_timer_get_time();
            size_t left = static_cast<size_t>(r);
            while (n > 0 && left >= iov->iov_len) {
                left -= iov->iov_len;
                ++iov;
                --n;
            }
            if (n > 0) {
                iov->iov_base = static_cast<uint8_t*>(iov->iov_base) + left;
                iov->iov_len -= left;
            }
            continue;
        }
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return Io::Closed;
        const int64_t waited = esp_timer_get_time() - last_progress;
        if (waited >= kSendStallUs) {
            ESP_LOGW(TAG, "send stalled %lld ms; dropping client", static_cast<long long>(waited / 1000));
            return Io::Closed;
        }
        fd_set wr, rd;
        FD_ZERO(&wr);
        FD_ZERO(&rd);
        FD_SET(fd, &wr);
        FD_SET(listen_fd, &rd);
        timeval tv = {0, 100'000};
        if (select(std::max(fd, listen_fd) + 1, &rd, &wr, nullptr, &tv) > 0 && FD_ISSET(listen_fd, &rd))
            return Io::Preempted;
    }
    return Io::Ok;
}

// ------------------------------------------------------------- session ----

struct Client {
    int fd = -1;
    uint8_t hello[streamer::kHelloSize];
    size_t hello_len = 0;
    bool started = false;  // valid HELLO received
    int credits = 0;
    uint16_t frame_seq = 0;
    uint16_t status_seq = 0;
    int64_t connected_us = 0;
    int64_t last_rx_us = 0;
    int64_t next_status_us = 0;
    // 1 s window for STATUS
    uint32_t st_sent = 0;
    uint32_t st_grabs = 0;
    int64_t st_start_us = 0;
    uint32_t last_len = 0;
};

class Server {
public:
    void run();

private:
    bool open_listener();
    void accept_client();
    void drop_client(const char* why);
    void replace_client();
    bool read_client();  // false: protocol error or closed
    bool send_status(int64_t now);
    bool send_frame();
    void idle_grab();
    void maybe_report(int64_t now);

    int listen_fd_ = -1;
    Client c_;
    Window win_;
};

bool Server::open_listener() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd_ < 0) return false;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(streamer::kVideoPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listen_fd_, 2) != 0 ||
        !set_nonblocking(listen_fd_)) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    ESP_LOGI(TAG, "listening on tcp %u", streamer::kVideoPort);
    return true;
}

void Server::accept_client() {
    sockaddr_in from = {};
    socklen_t flen = sizeof from;
    int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&from), &flen);
    if (fd < 0) return;
    if (c_.fd >= 0) drop_client("replaced by new connection");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (!set_nonblocking(fd)) {
        close_hard(fd);
        return;
    }
    const int64_t now = esp_timer_get_time();
    c_ = Client{};
    c_.fd = fd;
    c_.connected_us = now;
    c_.last_rx_us = now;
    char ip[16];
    inet_ntoa_r(from.sin_addr, ip, sizeof ip);
    ESP_LOGI(TAG, "client %s:%u connected", ip, ntohs(from.sin_port));
}

void Server::drop_client(const char* why) {
    if (c_.fd < 0) return;
    ESP_LOGI(TAG, "client dropped: %s (sent %u frames)", why, c_.frame_seq);
    close_hard(c_.fd);
    c_ = Client{};
}

// The old client is mid-message, so it must go even if the new accept fails.
void Server::replace_client() {
    drop_client("replaced by new connection");
    accept_client();
}

bool Server::read_client() {
    uint8_t buf[64];
    for (;;) {
        ssize_t n = recv(c_.fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n == 0) return false;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
        c_.last_rx_us = esp_timer_get_time();
        for (ssize_t i = 0; i < n; ++i) {
            if (!c_.started) {
                c_.hello[c_.hello_len++] = buf[i];
                if (c_.hello_len < streamer::kHelloSize) continue;
                streamer::Hello h;
                if (!streamer::unpack_hello(c_.hello, sizeof c_.hello, h)) {
                    ESP_LOGW(TAG, "invalid HELLO");
                    return false;
                }
                ESP_LOGI(TAG, "HELLO fs %u q %u credits %u", h.framesize, h.quality, h.credits);
                // Reconfiguring blocks this task for a few SCCB writes; fine, no frame is in flight yet.
                if (camera_source::configure(h.framesize, h.quality) != ESP_OK) {
                    ESP_LOGW(TAG, "camera configuration failed; rejecting HELLO");
                    return false;
                }
                c_.started = true;
                c_.credits = h.credits;
                c_.next_status_us = 0;  // send one right away
                c_.st_start_us = esp_timer_get_time();
            } else if (buf[i] == streamer::kCredit) {
                c_.credits = std::min<int>(c_.credits + 1, streamer::kMaxCredits);
            } else {
                ESP_LOGW(TAG, "unexpected byte 0x%02x from client", buf[i]);
                return false;
            }
        }
    }
}

bool Server::send_status(int64_t now) {
    const int64_t dt = now - c_.st_start_us;
    char json[streamer::kStatusMax];
    int len = snprintf(json, sizeof json,
                       "{\"fps\":%.1f,\"cam_fps\":%.1f,\"kb\":%.1f,"
                       "\"heap\":%u,\"psram\":%u,\"uptime_s\":%lld,\"sensor\":\"%s\",\"fs\":%u,\"q\":%u,"
                       "\"wb\":\"%s\"}",
                       per_sec(c_.st_sent, dt), per_sec(c_.st_grabs, dt), c_.last_len / 1024.0f,
                       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                       static_cast<long long>(now / 1'000'000), camera_source::sensor_name(),
                       camera_source::framesize(), camera_source::quality(),
                       camera_source::wb_control_name(camera_source::wb_control()));
    if (len < 0 || len >= static_cast<int>(sizeof json)) return true;  // cannot happen with these fields; skip
    c_.st_sent = c_.st_grabs = 0;
    c_.st_start_us = now;
    uint8_t hdr[streamer::kHeaderSize];
    streamer::pack_header(hdr, streamer::kTypeStatus, ++c_.status_seq, 0, static_cast<uint32_t>(len));
    iovec iov[2] = {{hdr, sizeof hdr}, {json, static_cast<size_t>(len)}};
    Io r = send_all(c_.fd, listen_fd_, iov, 2);
    if (r == Io::Preempted) replace_client();
    return r != Io::Closed;
}

bool Server::send_frame() {
    // Grab only now that a credit is held. camera_source copies the frame and
    // hands the driver buffer straight back, so with CAMERA_GRAB_LATEST the
    // driver keeps capturing during the send and this is the newest frame.
    camera_source::Frame f;
    if (!camera_source::grab(f, kGrabWaitMs)) return true;
    const int64_t now = esp_timer_get_time();
    win_.on_grab(f.len);
    ++c_.st_grabs;
    if (f.len > streamer::kMaxPayload) return true;
    uint8_t hdr[streamer::kHeaderSize];
    streamer::pack_header(hdr, streamer::kTypeFrame, ++c_.frame_seq, f.width, f.len,
                      static_cast<uint32_t>(std::max<int64_t>(0, now - f.timestamp_us)));
    iovec iov[2] = {{hdr, sizeof hdr}, {const_cast<uint8_t*>(f.data), f.len}};
    Io r = send_all(c_.fd, listen_fd_, iov, 2);
    tap(f);  // f.data stays valid until the next grab
    if (r == Io::Preempted) {
        replace_client();
        return true;
    }
    if (r == Io::Closed) return false;
    --c_.credits;
    ++win_.sent;
    ++c_.st_sent;
    c_.last_len = f.len;
    return true;
}

void Server::idle_grab() {
    if (tap_wanted()) {
        // grab() instead of skip() only to get the bytes; same frame, same stats.
        camera_source::Frame f;
        if (!camera_source::grab(f, kGrabWaitMs)) return;
        win_.on_grab(f.len);
        tap(f);
        return;
    }
    uint32_t len = 0;
    if (camera_source::skip(len, kGrabWaitMs)) win_.on_grab(len);
}

void Server::maybe_report(int64_t now) {
    const int64_t dt = now - win_.start_us;
    if (dt < kStatsPeriodUs) return;
    Report r;
    r.cam_fps = per_sec(win_.grabs, dt);
    r.sent_fps = per_sec(win_.sent, dt);
    r.avg_kb = win_.grabs ? static_cast<float>(win_.bytes) / win_.grabs / 1024.0f : 0;
    r.max_kb = win_.max_len / 1024.0f;
    r.client = c_.fd >= 0;
    char line[256];
    format_line(line, sizeof line, r);
    ESP_LOGI(TAG, "%s", line);
    win_.reset(now);
}

void Server::run() {
    while (!open_listener()) {
        ESP_LOGE(TAG, "listen failed: errno %d; retrying", errno);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    win_.reset(esp_timer_get_time());
    for (;;) {
        int64_t now = esp_timer_get_time();
        maybe_report(now);

        if (c_.fd < 0) {
            // Idle: no client. Keep the camera running and measured; poll accept
            // between frames (the camera wait paces this loop at the sensor's frame rate).
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(listen_fd_, &rd);
            timeval tv = {0, camera_source::ready() ? 0 : 100'000};
            if (select(listen_fd_ + 1, &rd, nullptr, nullptr, &tv) > 0) accept_client();
            else idle_grab();
            continue;
        }

        // Connected. Wait for input only when there is nothing to send.
        const bool can_send = c_.started && c_.credits > 0 && camera_source::ready();
        int64_t wait_us = 0;
        if (!can_send) {
            int64_t until = win_.start_us + kStatsPeriodUs;
            if (c_.started) until = std::min(until, c_.next_status_us);
            wait_us = std::clamp<int64_t>(until - now, 1000, 100'000);
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listen_fd_, &rd);
        FD_SET(c_.fd, &rd);
        timeval tv = {0, static_cast<suseconds_t>(wait_us)};
        if (select(std::max(listen_fd_, c_.fd) + 1, &rd, nullptr, nullptr, &tv) > 0) {
            if (FD_ISSET(listen_fd_, &rd)) {
                accept_client();
                continue;
            }
            if (FD_ISSET(c_.fd, &rd) && !read_client()) {
                drop_client(c_.started ? "closed or protocol error" : "closed before/invalid HELLO");
                continue;
            }
        }
        now = esp_timer_get_time();
        if (!c_.started) {
            if (now - c_.connected_us > kHelloTimeoutUs) drop_client("no HELLO within 2 s");
            continue;
        }
        if (c_.credits == 0 && now - c_.last_rx_us > kIdleTimeoutUs) {
            drop_client("no credit for 10 s");
            continue;
        }
        if (now >= c_.next_status_us) {
            c_.next_status_us = now + kStatusPeriodUs;
            if (!send_status(now)) {
                drop_client("status send failed");
                continue;
            }
        }
        if (c_.fd >= 0 && c_.credits > 0 && camera_source::ready() && !send_frame()) {
            drop_client("frame send failed");
        }
    }
}

Server s_server;

void server_task(void*) { s_server.run(); }

}  // namespace

void start() {
    s_tap_buf = static_cast<uint8_t*>(heap_caps_malloc(streamer::kMaxPayload, MALLOC_CAP_SPIRAM));
    xTaskCreatePinnedToCore(server_task, "video_tx", 6144, nullptr, kPrio, nullptr, 0);
}

bool sample(Sample& out, uint32_t timeout_ms) {
    if (!s_tap_buf || !camera_source::ready()) return false;
    const uint32_t want = s_tap_req.fetch_add(1) + 1;
    const TickType_t start = xTaskGetTickCount();
    while (s_tap_done.load() != want) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    out = {s_tap_buf, s_tap_len, s_tap_w, s_tap_h, s_tap_us};
    return true;
}

}  // namespace video_tx
