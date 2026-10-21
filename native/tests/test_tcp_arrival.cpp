#include <cassert>
#include <cstdint>
#include <cstdio>
#include "tcp_arrival.hpp"

int main() {
    native_badge::TcpArrivalWindow window;
    window.observe(100, 100, 0, 1);  // ACK-only packets are not payload arrivals
    assert(window.snapshot(2).segments == 0);
    window.observe(100, 100, 1460, 0);
    window.observe(3000, 1560, 1460, 20);
    window.observe(4460, 1560, 1460, 30);
    window.observe(100, 1560, 1460, 40);  // entirely behind, ends before expected byte
    auto s = window.snapshot(50);
    assert(s.first_raw_us == 0 && s.segments == 4 && s.ahead == 2 && s.behind == 1);
    assert(s.max_ahead_bytes == 2900 && s.pending_ahead_us == 30 && s.cover_events == 0);
    window.observe(1000, 1560, 1460, 60);  // overlap covers expected byte
    s = window.snapshot(70);
    assert(s.behind == 2 && s.cover_events == 1 && s.cover_wait_max_us == 40 && !s.pending_ahead_us);
    window.observe(6000, 4460, 1460, 80);
    window.observe(4460, 4460, 1460, 180);
    assert(window.snapshot(190).cover_wait_max_us == 100);

    window.reset();
    assert(!window.snapshot(200).segments);
    window.observe(20, UINT32_MAX - 10, 100, UINT32_MAX - 5);
    s = window.snapshot(5);
    assert(s.ahead == 1 && s.max_ahead_bytes == 31 && s.pending_ahead_us == 11);
    window.observe(UINT32_MAX - 20, UINT32_MAX - 10, 100, 15);
    s = window.snapshot(20);
    assert(s.behind == 1 && s.cover_events == 1 && s.cover_wait_max_us == 21 && !s.pending_ahead_us);
    std::puts("native TCP arrival: ordering, partial overlap, reset and sequence/timer wrap passed");
}
