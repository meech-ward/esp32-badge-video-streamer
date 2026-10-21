#include <cassert>
#include <cstdint>
#include <cstdio>
#include "link_policy.hpp"

int main() {
    native_badge::LinkRecovery p;
    assert(!p.poll(0, true, 0, 0));
    assert(!p.poll(1000, true, 0, 1));
    assert(!p.poll(2000, true, 0, 2));
    assert(p.poll(3000, true, 0, 3));  // UP can be stale after an AP reset
    p.reset();
    assert(!p.poll(4000, true, 1, 3));
    assert(!p.poll(11000, true, 2, 3));  // a new complete frame resets the timer
    assert(!p.poll(18999, true, 2, 3));
    assert(p.poll(19000, true, 2, 3));  // STATUS alone is not live video
    assert(!p.poll(20000, false, 2, 3));
    assert(!p.poll(21000, true, 2, 3));  // a real reassociation gets a fresh window
    p.reset();
    assert(!p.poll(UINT32_MAX - 4000, true, 2, UINT32_MAX - 1));
    assert(!p.poll(3998, true, 2, 0));
    assert(p.poll(3999, true, 2, 1));  // timers and failure counts wrap safely
    std::puts("native link: stale-UP retries, frame freshness, reset and wrap passed");
}
