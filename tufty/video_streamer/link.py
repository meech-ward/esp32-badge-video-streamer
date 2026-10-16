"""Wi-Fi + video link supervision. Device-independent.

CYW43 can keep reporting an association after the ESP's SoftAP has gone and
come back (an ESP reboot drops the AP for about a second with no deauth). The
badge then sends into a link that carries nothing, forever. The ESP sends a
STATUS about once per second on the video stream whatever the credits, so any
LINK_DEAD_MS without a received byte while Wi-Fi claims to be up means the
link is dead, and Wi-Fi is reset.

Silence is judged in two regimes:

- Bytes have flowed since this join, then stopped: the association is stale
  (or the ESP is gone). LINK_DEAD_MS applies.
- No byte at all since this join: usually not a stale association but a lost
  first SYN (seen on the original hardware setup). The stream fixes that itself with a fresh socket after
  stream.CONNECT_MS, so Wi-Fi is only cycled after NEVER_RX_DEAD_MS, which
  spans several such attempts.

`wifi` needs: poll(now) -> bool, reset(now, reason). `stream` is stream.Stream.
"""
# WHY 2500: STATUS arrives about every 1 s, so 2.5 s is two missed STATUS
# plus margin. Measured recovery after an ESP reboot with 4000 was 4 s
# detection + ~3 s Wi-Fi join + <0.2 s to the first frame; detection was the
# largest part. Only used once a byte has arrived since the join.
LINK_DEAD_MS = 2500
# WHY 8000: before the first byte, the stream retries a hung connect every
# stream.CONNECT_MS + BACKOFF_MIN_MS (1250 ms), so 8 s is about six attempts:
# enough that a lost SYN is retried rather than treated as a dead association
# (which cost ~6 s per occurrence: 0.5 s interface cycle + ~3 s rejoin, plus
# the 2.5 s already waited), while an association that really is stale from
# the start still gets reset in ~8 s + the cycle and rejoin.
NEVER_RX_DEAD_MS = 8000


class Link:
    def __init__(self, wifi, stream, ticks_diff, dead_ms=LINK_DEAD_MS, never_rx_ms=NEVER_RX_DEAD_MS):
        self.wifi = wifi
        self.stream = stream
        self.ticks_diff = ticks_diff
        self.dead_ms = dead_ms
        self.never_rx_ms = never_rx_ms
        self.up = False
        # The clock starts when Wi-Fi comes up, so the first TCP connect gets
        # the full window; time in failed or pending connects counts toward it.
        self.t_alive = None
        self.rx_seen = 0
        self.rx_since_join = False  # selects the window: never_rx_ms until the first byte
        self.suspect = False
        self.joins = 0
        self.dead_resets = 0

    def poll(self, now):
        """True while Wi-Fi is up and the link has not been judged dead."""
        up = self.wifi.poll(now)
        s = self.stream
        if not up:
            self.up = False
            self.t_alive = None
            return False
        if not self.up:
            self.up = True
            self.joins += 1
            # A rejoin usually means the ESP just came back: reconnect fast.
            s.reset_backoff()
            self.t_alive = now
            self.rx_seen = s.bytes
            self.rx_since_join = False
            self.suspect = False
            return True
        if s.bytes != self.rx_seen:
            self.rx_seen = s.bytes
            self.t_alive = now
            self.rx_since_join = True
            self.suspect = False
            return True
        silent = self.ticks_diff(now, self.t_alive)
        if silent < (self.dead_ms if self.rx_since_join else self.never_rx_ms):
            return True
        if not self.suspect:
            # This runs before the caller drains the socket, so after a long
            # loop stall the proof of life may still sit unread in lwIP. Give
            # the stream one poll before judging.
            self.suspect = True
            return True
        self.dead_resets += 1
        s.close("link dead", backoff=False)
        if self.rx_since_join:
            reason = "no data for %d ms" % silent
        else:
            reason = "no data for %d ms since join" % silent
        self.wifi.reset(now, reason)
        self.up = False
        self.t_alive = None
        self.suspect = False
        return False
