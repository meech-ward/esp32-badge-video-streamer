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
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "streamer/protocol.hpp"
#include "streamer/status_json.hpp"

namespace video_tx {
namespace {

constexpr const char* TAG = "video";
// Below lwIP (18): frame sending must never starve the network stack.
constexpr UBaseType_t kPrio = 5;

constexpr int64_t kStatusPeriodUs = 1'000'000;
constexpr int64_t kStatsPeriodUs = 2'000'000;
constexpr int64_t kHelloTimeoutUs = 2'000'000;
// A send that makes no progress for this long means the client is gone or stuck.
constexpr int64_t kSendStallUs = 1'500'000;
// A client that holds no credits and sends nothing for this long is dropped,
// returning the camera to the idle loop.
constexpr int64_t kIdleTimeoutUs = 10'000'000;
// Longest one camera wait may block this task. Short enough that STATUS (1 s),
// accept preemption and the timeouts above stay on time if the sensor stalls;
// a slower frame just means the loop goes round and waits again.
constexpr uint32_t kGrabWaitMs = 100;

// ---------------------------------------------------------------- stats ----

// Only the peer IP is handed off. The latest request replaces an old one;
// no periodic diagnostic snapshots or duplicate timing counters are kept.
QueueHandle_t s_rssi_queue = nullptr;
// Wi-Fi/DHCP inspection runs only in the low-priority report task. STATUS
// merely loads the last result, never a synchronous radio/netif operation.
std::atomic<uint32_t> s_rssi_peer_ip{0};
std::atomic<int> s_rssi{-127};
portMUX_TYPE s_rssi_mux = portMUX_INITIALIZER_UNLOCKED;

void set_rssi_peer(uint32_t peer_ip) {
    // A late SDK sample for the previous client must never replace the new
    // client's unknown RSSI. Serialize peer changes with the checked publish;
    // no SDK call or formatting occurs inside this short critical section.
    taskENTER_CRITICAL(&s_rssi_mux);
    s_rssi.store(-127);
    s_rssi_peer_ip.store(peer_ip);
    taskEXIT_CRITICAL(&s_rssi_mux);
}

void publish_rssi(uint32_t peer_ip, int rssi) {
    taskENTER_CRITICAL(&s_rssi_mux);
    if (s_rssi_peer_ip.load() == peer_ip) s_rssi.store(rssi);
    taskEXIT_CRITICAL(&s_rssi_mux);
}

// ------------------------------------------------------------ sample ----

// Soft AWB requests a frame through a request/done handshake, filled from a
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
Io send_all(int fd, int listen_fd, iovec* iov, int n, uint64_t* wait_us = nullptr) {
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
        const int64_t wait_start = esp_timer_get_time();
        const int ready = select(std::max(fd, listen_fd) + 1, &rd, &wr, nullptr, &tv);
        if (wait_us) *wait_us += static_cast<uint64_t>(esp_timer_get_time() - wait_start);
        if (ready > 0 && FD_ISSET(listen_fd, &rd)) return Io::Preempted;
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
    uint32_t max_frame_bytes = streamer::kMaxPayload;
    uint32_t drop_oversize = 0;
    uint16_t frame_seq = 0;
    uint16_t status_seq = 0;
    int64_t connected_us = 0;
    int64_t last_rx_us = 0;
    int64_t next_status_us = 0;
    // 1 s window for STATUS
    uint32_t st_sent = 0;
    uint32_t st_grabs = 0;
    int64_t st_start_us = 0;
    uint32_t peer_ip = 0;
    uint32_t last_len = 0;
    streamer::StreamTimings timing;
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
    void maybe_sample_rssi(int64_t now);

    int listen_fd_ = -1;
    Client c_;
    int64_t next_rssi_us_ = 0;
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
    c_.peer_ip = from.sin_addr.s_addr;
    set_rssi_peer(c_.peer_ip);
    char ip[16];
    inet_ntoa_r(from.sin_addr, ip, sizeof ip);
    ESP_LOGI(TAG, "client %s:%u connected", ip, ntohs(from.sin_port));
}

void Server::drop_client(const char* why) {
    if (c_.fd < 0) return;
    ESP_LOGI(TAG, "client dropped: %s (sent %u frames)", why, c_.frame_seq);
    close_hard(c_.fd);
    c_ = Client{};
    set_rssi_peer(0);
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
                if (camera_source::configure(h.framesize, h.quality) != ESP_OK) return false;
                c_.started = true;
                c_.credits = h.credits;
                c_.max_frame_bytes = h.payload_limit();
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
    streamer::VideoStatus status;
    status.fps = per_sec(c_.st_sent, dt);
    status.grab_fps = per_sec(c_.st_grabs, dt);
    status.last_kb = c_.last_len / 1024.0f;
    status.heap = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    status.psram = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    status.uptime_s = now / 1'000'000;
    status.sensor = camera_source::sensor_name();
    status.framesize = camera_source::framesize();
    status.quality = camera_source::quality();
    status.wb = camera_source::wb_control_name(camera_source::wb_control());
    status.timing = c_.timing;
    status.drop_oversize = c_.drop_oversize;
    status.rssi = s_rssi.load();
    const int len = streamer::format_video_status(json, sizeof json, status);
    if (len < 0 || len >= static_cast<int>(sizeof json)) return true;  // cannot happen with these fields; skip
    c_.st_sent = c_.st_grabs = 0;
    c_.timing = {};
    c_.st_start_us = now;
    uint8_t hdr[streamer::kHeaderSize];
    streamer::pack_header(hdr, streamer::kTypeStatus, ++c_.status_seq, 0, static_cast<uint32_t>(len), 0);
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
    const int64_t grab_start = esp_timer_get_time();
    const bool grabbed = camera_source::grab(f, kGrabWaitMs);
    const int64_t now = esp_timer_get_time();
    const uint64_t grab_us = static_cast<uint64_t>(now - grab_start);
    c_.timing.grab.add(grab_us);
    if (!grabbed) return true;
    ++c_.st_grabs;
    c_.timing.frame(f.len);
    if (f.len > c_.max_frame_bytes) {
        // This client cannot use the JPEG. Drop before touching TCP, preserving
        // both its credit and wire sequence; the next grab gets a newer frame.
        // Return to run() so STATUS/accept remain serviced even if every frame
        // exceeds the cap. No extra waiting or queue is introduced.
        ++c_.drop_oversize;
        // AWB still needs its requested sample even when this client cannot
        // display it; otherwise sustained oversize captures freeze colour.
        tap(f);
        return true;
    }
    uint8_t hdr[streamer::kHeaderSize];
    streamer::pack_header(hdr, streamer::kTypeFrame, ++c_.frame_seq, f.width, f.len,
                      static_cast<uint32_t>(std::max<int64_t>(0, now - f.timestamp_us)));
    iovec iov[2] = {{hdr, sizeof hdr}, {const_cast<uint8_t*>(f.data), f.len}};
    const int64_t send_start = esp_timer_get_time();
    uint64_t wait_us = 0;
    Io r = send_all(c_.fd, listen_fd_, iov, 2, &wait_us);
    const int64_t send_end = esp_timer_get_time();
    const uint64_t send_us = static_cast<uint64_t>(send_end - send_start);
    c_.timing.send.add(send_us);
    c_.timing.wait.add(wait_us);
    tap(f);  // f.data stays valid until the next grab
    if (r == Io::Preempted) {
        replace_client();
        return true;
    }
    if (r == Io::Closed) return false;
    --c_.credits;
    ++c_.st_sent;
    c_.last_len = f.len;
    return true;
}

void Server::idle_grab() {
    if (tap_wanted()) {
        // Copy only when software AWB has requested a sample.
        camera_source::Frame f;
        if (!camera_source::grab(f, kGrabWaitMs)) return;
        tap(f);
        return;
    }
    uint32_t len = 0;
    camera_source::skip(len, kGrabWaitMs);
}

void Server::maybe_sample_rssi(int64_t now) {
    if (now < next_rssi_us_) return;
    if (s_rssi_queue) xQueueOverwrite(s_rssi_queue, &c_.peer_ip);
    next_rssi_us_ = now + kStatsPeriodUs;
}

void Server::run() {
    while (!open_listener()) {
        ESP_LOGE(TAG, "listen failed: errno %d; retrying", errno);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    for (;;) {
        int64_t now = esp_timer_get_time();
        maybe_sample_rssi(now);

        if (c_.fd < 0) {
            // Idle: no client. Keep the camera running; poll accept
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
            int64_t until = next_rssi_us_;
            if (c_.started) until = std::min(until, c_.next_status_us);
            wait_us = std::clamp<int64_t>(until - now, 1000, 100'000);
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listen_fd_, &rd);
        FD_SET(c_.fd, &rd);
        timeval tv = {0, static_cast<suseconds_t>(wait_us)};
        const bool credit_wait = c_.started && c_.credits == 0;
        const int64_t select_start = esp_timer_get_time();
        const int ready = select(std::max(listen_fd_, c_.fd) + 1, &rd, nullptr, nullptr, &tv);
        if (credit_wait) {
            const uint64_t waited = static_cast<uint64_t>(esp_timer_get_time() - select_start);
            c_.timing.credit_wait_us += waited;
        }
        if (ready > 0) {
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

int sample_rssi(uint32_t peer_ip) {
    if (!peer_ip) return -127;
    wifi_sta_list_t stations{};
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num <= 0) return -127;
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return -127;
    esp_netif_pair_mac_ip_t clients[ESP_WIFI_MAX_CONN_NUM]{};
    for (int i = 0; i < stations.num; ++i) std::memcpy(clients[i].mac, stations.sta[i].mac, 6);
    if (esp_netif_dhcps_get_clients_by_mac(ap, stations.num, clients) != ESP_OK) return -127;
    for (int i = 0; i < stations.num; ++i)
        if (clients[i].ip.addr == peer_ip) return stations.sta[i].rssi;
    return -127;  // never report a different associated client's signal
}

void rssi_task(void*) {
    uint32_t peer_ip;
    for (;;) {
        if (xQueueReceive(s_rssi_queue, &peer_ip, portMAX_DELAY) == pdTRUE) {
            const int rssi = sample_rssi(peer_ip);
            // A reconnect may replace the video client during the SDK calls.
            publish_rssi(peer_ip, rssi);
        }
    }
}

}  // namespace

void start() {
    s_rssi_queue = xQueueCreate(1, sizeof(uint32_t));
    if (s_rssi_queue && xTaskCreatePinnedToCore(rssi_task, "video_rssi", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
        vQueueDelete(s_rssi_queue);
        s_rssi_queue = nullptr;  // stream still works; RSSI stays unknown
    }
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
