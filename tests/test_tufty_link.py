"""Link liveness (tufty/video_streamer/link.py) with net.Wifi on a fake CYW43 and the real Stream."""
import errno
import importlib
import sys
import types

import pytest

import link as L  # noqa: E402
import stream as S  # noqa: E402
import stream_protocol as P  # noqa: E402

LINK_UP = 3  # CYW43_LINK_UP


class FakeWLAN:
    """status()/isconnected() modelled on network_cyw43.c: both read the netif.

    `stale` keeps the link "up" after disconnect(), as when the AP vanished
    without a deauth; only active(False) (netif removed) clears it.
    """
    PM_NONE = 0x10

    def __init__(self, iface):
        self._active = False
        self.link = 0
        self.stale = False
        self.join_ok = True
        self.calls = []

    def active(self, v=None):
        if v is None:
            return self._active
        self.calls.append(("active", v))
        self._active = v
        if not v:
            self.link = 0

    def config(self, **kw):
        self.calls.append(("pm", kw["pm"]))

    def connect(self, ssid, key):
        self.calls.append(("connect", ssid))
        self.link = LINK_UP if self.join_ok else 1

    def disconnect(self):
        self.calls.append(("disconnect",))
        if not self.stale:
            self.link = 0

    def isconnected(self):
        return self.link == LINK_UP

    def status(self):
        return self.link

    def ifconfig(self):
        return ("192.168.4.2", "255.255.255.0", "192.168.4.1", "0.0.0.0")


@pytest.fixture
def net(monkeypatch):
    network = types.SimpleNamespace(STA_IF=0, STAT_WRONG_PASSWORD=-3, STAT_NO_AP_FOUND=-2,
                                    STAT_CONNECT_FAIL=-1, STAT_GOT_IP=3, WLAN=FakeWLAN)
    fsocket = types.SimpleNamespace(AF_INET=2, SOCK_STREAM=1,
                                    getaddrinfo=lambda h, p: [(2, 1, 0, "", (h, p))])
    monkeypatch.setitem(sys.modules, "network", network)
    monkeypatch.setitem(sys.modules, "socket", fsocket)
    sys.modules.pop("net", None)
    mod = importlib.import_module("net")
    yield mod
    sys.modules.pop("net", None)


class Clock:
    def __init__(self):
        self.t = 0

    def ticks_ms(self):
        return self.t

    @staticmethod
    def ticks_diff(a, b):
        return a - b


class EspSock:
    """Connected video socket. The ESP side pushes a STATUS every second, or nothing if dead.

    `hang`: the SYN was lost, so the socket never becomes writable (send keeps
    raising EAGAIN, as lwIP does while a connect is in progress).
    """

    def __init__(self, clock, esp, hang=False):
        self.clock = clock
        self.esp = esp
        self.hang = hang
        self.t_open = clock.t
        self.out = bytearray()
        self.t_status = None
        self.closed = False
        self.seq = 0

    def send(self, buf):
        if self.hang:
            raise OSError(errno.EAGAIN)  # connect never completes
        return len(buf)

    def readinto(self, buf):
        now = self.clock.t
        if self.esp.alive and (self.t_status is None or now - self.t_status >= 1000):
            self.t_status = now
            self.seq += 1
            body = b'{"fps":0}'
            self.out += P.pack_header(P.TYPE_STATUS, self.seq, 0, len(body)) + body
        if not self.out:
            return None
        n = min(len(buf), len(self.out))
        buf[:n] = self.out[:n]
        del self.out[:n]
        return n

    def close(self):
        self.closed = True


def rig(net, alive=True, hang=0, open_error=False):
    """`hang`: how many sockets, from the first, never complete their connect."""
    clock = Clock()
    logs = []
    log = logs.append
    socks = []
    esp = types.SimpleNamespace(alive=alive)

    def open_socket():
        if open_error:
            raise OSError(errno.EHOSTUNREACH)
        socks.append(EspSock(clock, esp, hang=len(socks) < hang))
        return socks[-1]
    wifi = net.Wifi("tufty-video", "pw", clock.ticks_diff, log=log)
    resets_at = []  # clock value of every forced Wi-Fi reset
    reset = wifi.reset

    def timed_reset(now, reason):
        resets_at.append(now)
        reset(now, reason)
    wifi.reset = timed_reset
    st = S.Stream(open_socket, clock.ticks_ms, clock.ticks_diff, backoff_max_ms=1000, log=log)
    lk = L.Link(wifi, st, clock.ticks_diff)
    return types.SimpleNamespace(clock=clock, esp=esp, logs=logs, socks=socks, wifi=wifi, wlan=wifi.wlan,
                                 stream=st, link=lk, resets_at=resets_at)


def run(r, ms, step=20):
    """Poll like App.update: link first, then the stream while the link is up."""
    downs = 0
    end = r.clock.t + ms
    while r.clock.t < end:
        now = r.clock.t
        if r.link.poll(now):
            if r.stream.poll(now):
                r.stream.frame_drawn()
        else:
            downs += 1
            if r.stream.state != S.IDLE:
                r.stream.close("wifi down", backoff=False)
        r.clock.t += step
    return downs


def run_to(r, t_end, step=20):
    """Like run(), but the last poll happens at exactly t_end (a multiple of step)."""
    assert (t_end - r.clock.t) % step == 0
    return run(r, t_end - r.clock.t + step, step)


def test_healthy_link_between_frames_never_resets(net):
    r = rig(net)
    run(r, 60_000)
    assert r.link.dead_resets == 0 and r.wifi.resets == 0
    assert r.stream.connected() and r.stream.statuses >= 59 and r.stream.frames == 0
    assert r.stream.connects == 1


def test_long_loop_stall_on_a_live_link_does_not_reset(net):
    r = rig(net)
    run(r, 3000)
    assert r.stream.connected()
    # One update arrives 5 s late; the STATUS proving the link alive is still unread.
    run(r, 5000, step=5000)
    run(r, 2000)
    assert r.link.dead_resets == 0 and r.wifi.resets == 0 and r.stream.connected()


def test_stale_association_is_reset_and_rejoined(net):
    r = rig(net)
    run(r, 3000)
    assert r.stream.connected() and r.link.rx_since_join
    # The ESP reboots: its stream goes quiet, but CYW43 keeps the stale link up.
    r.wlan.stale = True
    r.esp.alive = False
    t_last = r.link.t_alive  # the poll that saw the last byte
    run(r, L.LINK_DEAD_MS - 1000)
    assert r.wifi.resets == 0  # within the window: no flapping
    n = len(r.wlan.calls)
    # Bytes had flowed, so the short window applies: at exactly LINK_DEAD_MS of
    # silence the link is suspect, and the next poll resets.
    run_to(r, t_last + L.LINK_DEAD_MS)
    assert r.wifi.resets == 0 and r.link.suspect
    run(r, 20)
    assert r.wifi.resets == 1 and r.link.dead_resets == 1
    assert r.resets_at == [t_last + L.LINK_DEAD_MS + 20]
    run(r, 100)  # the rejoin follows on the next polls
    calls = r.wlan.calls[n:]
    # disconnect() did not clear the stale link, so the interface was cycled, then rejoined.
    assert calls[:3] == [("disconnect",), ("active", False), ("active", True)]
    assert ("connect", "tufty-video") in calls and calls.index(("connect", "tufty-video")) > 2
    assert r.wifi.cycles == 1
    assert any(m.startswith("wifi reset: no data for") and m.endswith("(cycled)") for m in r.logs)
    assert any(m.startswith("tcp close link dead") for m in r.logs)
    # The ESP is back: after the rejoin the stream reconnects and stays up.
    r.wlan.stale = False
    run(r, 500)
    r.esp.alive = True
    run(r, 10_000)
    assert r.stream.connected() and r.wifi.resets == 1


def test_disconnect_that_clears_the_link_skips_the_cycle(net):
    r = rig(net)
    run(r, 2000)
    r.esp.alive = False
    n = len(r.wlan.calls)
    run(r, L.LINK_DEAD_MS + 100)
    calls = r.wlan.calls[n:]
    assert calls[0] == ("disconnect",) and ("active", False) not in calls
    assert r.wifi.resets == 1 and r.wifi.cycles == 0


def test_clock_starts_at_wifi_up_and_failed_connects_count_toward_the_long_window(net):
    r = rig(net, open_error=True)
    r.wlan.join_ok = False  # AP not there yet: joining for a long time is not "dead"
    run(r, 10_000)
    assert r.wifi.resets == 0
    r.wifi.joining = False
    r.wlan.join_ok = True
    r.wifi.t = None
    run(r, 100)
    assert r.wifi.up
    t_up = r.clock.t
    run(r, L.LINK_DEAD_MS + 200)
    assert r.wifi.resets == 0  # no byte yet: the short window does not apply
    run(r, L.NEVER_RX_DEAD_MS - L.LINK_DEAD_MS - 400)
    assert r.wifi.resets == 0 and r.stream.errors >= 7  # failed connects so far, 1 s backoff cap
    run(r, 400)
    assert r.wifi.resets == 1 and r.clock.t - t_up >= L.NEVER_RX_DEAD_MS


def test_lost_first_syn_is_retried_by_the_stream_not_by_a_wifi_reset(net):
    """A lost SYN gets a fresh socket before the link watchdog cycles Wi-Fi."""
    r = rig(net, hang=1)
    step = 10  # CONNECT_MS + BACKOFF_MIN_MS = 1250 sits on a 10 ms poll grid
    run(r, 100, step)
    assert r.wifi.up and r.stream.state == S.CONNECTING and r.stream.connects == 1
    t_connect = r.socks[0].t_open
    run_to(r, t_connect + S.CONNECT_MS - step, step)
    assert r.stream.connects == 1 and r.stream.state == S.CONNECTING  # patient up to CONNECT_MS
    run(r, 2 * step, step)
    assert r.stream.connects == 1 and r.stream.state == S.BACKOFF and r.stream.reason == "connect timeout"
    assert r.socks[0].closed and r.stream.wait_ms == S.BACKOFF_MIN_MS
    run(r, 3000, step)
    assert r.stream.connects == 2 and r.stream.connected() and r.stream.statuses >= 1
    assert r.socks[1].t_open == t_connect + S.CONNECT_MS + S.BACKOFF_MIN_MS  # ~1.25 s, one retry
    assert r.stream.backoff_ms == S.BACKOFF_MIN_MS  # the timeout did not double it
    assert r.wifi.resets == 0 and r.link.dead_resets == 0 and r.link.joins == 1
    assert r.link.rx_since_join
    assert "tcp close connect timeout after 0 frames" in r.logs and "tcp connect #2" in r.logs
    assert not any(m.startswith("tcp close link dead") or m.startswith("wifi reset") for m in r.logs)
    # The link is healthy afterwards and the short window now applies, as before.
    run(r, 20_000)
    assert r.wifi.resets == 0 and r.stream.connects == 2


def test_never_connecting_link_is_reset_at_the_long_window_then_rejoin_loops(net):
    """ESP down for good (or the association stale from the start): every SYN hangs."""
    r = rig(net, hang=10 ** 6)
    r.wlan.stale = True
    step = 10
    run(r, 100, step)
    t_up = r.link.t_alive  # the poll that saw Wi-Fi up; connect #1 happened on the same poll
    assert r.wifi.up and r.socks[0].t_open == t_up
    run_to(r, t_up + L.NEVER_RX_DEAD_MS - step, step)
    assert r.wifi.resets == 0
    retry = S.CONNECT_MS + S.BACKOFF_MIN_MS
    assert r.stream.connects == 1 + (L.NEVER_RX_DEAD_MS - step) // retry >= 5  # several attempts
    assert [s.t_open - t_up for s in r.socks] == [retry * i for i in range(r.stream.connects)]
    assert all(m.endswith("connect timeout after 0 frames") for m in r.logs if m.startswith("tcp close"))
    run(r, step, step)  # exactly NEVER_RX_DEAD_MS of silence: suspect, one poll of grace
    assert r.wifi.resets == 0 and r.link.suspect
    run(r, step, step)
    assert r.wifi.resets == 1 and r.link.dead_resets == 1 and r.wifi.cycles == 1
    assert r.resets_at == [t_up + L.NEVER_RX_DEAD_MS + step]
    assert r.logs[-1] == "wifi reset: no data for %d ms since join (cycled)" % (L.NEVER_RX_DEAD_MS + step)
    assert "tcp close link dead after 0 frames" in r.logs
    # Rejoin loop: the stale association comes back the same, and is reset again
    # after another long window, never a short one.
    run(r, 100, step)
    assert r.link.joins == 2 and r.wifi.up and not r.link.rx_since_join
    run(r, L.NEVER_RX_DEAD_MS + 200, step)
    assert r.wifi.resets == 2 and r.link.joins == 3
    assert r.resets_at[1] - r.resets_at[0] > L.NEVER_RX_DEAD_MS
    assert r.stream.backoff_ms == S.BACKOFF_MIN_MS  # still no growth: every failure was a timeout


def test_dead_window_allows_two_missed_status():
    # STATUS arrives about every 1000 ms; two can be missed without a reset,
    # and detection stays well under the old 4000 ms.
    assert 2000 < L.LINK_DEAD_MS <= 2500


def test_never_rx_window_spans_several_connect_attempts():
    retry = S.CONNECT_MS + S.BACKOFF_MIN_MS  # a hung connect's retry period
    assert L.LINK_DEAD_MS < L.NEVER_RX_DEAD_MS
    assert 5 * retry <= L.NEVER_RX_DEAD_MS <= 10_000  # several retries, still a bounded wait
    assert S.CONNECT_MS < S.STALL_MS


def test_backoff_is_capped_while_up_and_reset_on_rejoin(net):
    r = rig(net, open_error=True)
    r.link.dead_ms = r.link.never_rx_ms = 10 ** 9  # isolate the backoff from the liveness reset
    run(r, 8000)
    assert r.stream.backoff_ms == 1000 and r.stream.wait_ms <= 1000
    r.wlan.link = 0  # real loss, then rejoin
    run(r, 100)
    assert r.link.joins == 2 and r.stream.backoff_ms <= 2 * S.BACKOFF_MIN_MS


def test_reset_backoff_skips_a_pending_wait():
    clock = Clock()
    st = S.Stream(lambda: (_ for _ in ()).throw(OSError(errno.EHOSTUNREACH)), clock.ticks_ms,
                  clock.ticks_diff, backoff_max_ms=1000)
    for _ in range(5):
        st.poll(clock.t)
        clock.t += 2000
    assert st.state == S.BACKOFF and st.backoff_ms == 1000
    st.reset_backoff()
    assert st.backoff_ms == S.BACKOFF_MIN_MS and st.wait_ms == 0


def test_wifi_transitions_are_logged(net):
    r = rig(net)
    run(r, 2000)
    r.wlan.link = 0
    run(r, 100)
    msgs = r.logs
    assert msgs[0] == "wifi join tufty-video"
    assert "wifi up 192.168.4.2" in msgs and "wifi lost" in msgs
    assert msgs.count("wifi join tufty-video") == 2
    assert "tcp connect #1" in msgs and any(m.startswith("tcp up in") for m in msgs)
    assert "tcp close wifi down after 0 frames" in msgs


def test_first_frame_logged_once_per_connect():
    clock = Clock()
    logs = []
    jpeg = b"\xff\xd8" + b"x" * 100 + b"\xff\xd9"
    data = bytearray()
    for i in range(3):
        data += P.pack_header(P.TYPE_FRAME, i + 1, 20, len(jpeg)) + jpeg

    class Sock:
        def send(self, buf):
            return len(buf)

        def readinto(self, buf):
            if not data:
                return None
            n = min(len(buf), len(data))
            buf[:n] = data[:n]
            del data[:n]
            return n

        def close(self):
            pass
    st = S.Stream(Sock, clock.ticks_ms, clock.ticks_diff, log=logs.append)
    for _ in range(10):
        if st.poll(clock.t):
            st.frame_drawn()
        clock.t += 5
    assert st.frames == 3
    assert [m for m in logs if m.startswith("first frame")] == ["first frame 104 B, 0 ms after connect"]
