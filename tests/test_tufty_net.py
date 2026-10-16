"""Host tests for tufty/video_streamer/net.py against fake `network`/`socket` modules."""
import errno
import importlib
import sys
import types

import pytest


class FakeWLAN:
    PM_NONE = 0x10  # extmod/network_cyw43.c: CYW43_PM_VALUE(NO_POWERSAVE, 10, 0, 0, 0)

    def __init__(self, iface):
        self._active = False
        self.connected = False
        self.st = 0
        self.calls = []
        self.pm = None

    def active(self, v=None):
        if v is None:
            return self._active
        self._active = v
        self.calls.append(("active", v))

    def config(self, **kw):
        self.pm = kw["pm"]
        self.calls.append(("pm", kw["pm"]))

    def connect(self, ssid, key):
        self.calls.append(("connect", ssid))
        self.st = 1

    def disconnect(self):
        self.calls.append(("disconnect",))
        self.connected = False

    def isconnected(self):
        return self.connected

    def status(self):
        return self.st

    def ifconfig(self):
        return ("192.168.4.2", "255.255.255.0", "192.168.4.1", "0.0.0.0")


class FakeSock:
    def __init__(self, family, kind):
        self.kind = kind
        self.opts = []
        self.blocking = True
        self.closed = False
        self.connect_error = errno.EINPROGRESS
        self.sent = []

    def setblocking(self, v):
        self.blocking = v

    def setsockopt(self, *a):
        self.opts.append(a)

    def connect(self, addr):
        self.addr = addr
        if self.connect_error:
            raise OSError(self.connect_error)

    def close(self):
        self.closed = True


@pytest.fixture
def net(monkeypatch):
    wlans = []
    network = types.SimpleNamespace(STA_IF=0, STAT_WRONG_PASSWORD=-3, STAT_NO_AP_FOUND=-2,
                                    STAT_CONNECT_FAIL=-1,
                                    WLAN=type("WLAN", (FakeWLAN,), {}))
    orig = network.WLAN.__init__

    def init(self, iface):
        orig(self, iface)
        wlans.append(self)
    network.WLAN.__init__ = init
    socks = []

    def mk(family, kind):
        socks.append(FakeSock(family, kind))
        return socks[-1]
    fsocket = types.SimpleNamespace(AF_INET=2, SOCK_STREAM=1, IPPROTO_TCP=6,
                                    TCP_NODELAY=1, socket=mk,
                                    getaddrinfo=lambda h, p: [(2, 1, 0, "", (h, p))])
    monkeypatch.setitem(sys.modules, "network", network)
    monkeypatch.setitem(sys.modules, "socket", fsocket)
    sys.modules.pop("net", None)
    mod = importlib.import_module("net")
    mod._video_addr = None
    mod.test_wlans, mod.test_socks = wlans, socks
    yield mod
    sys.modules.pop("net", None)


def diff(a, b):
    return a - b


def test_join_turns_power_save_off_before_and_after(net):
    w = net.Wifi("tufty-video", "pw", diff)
    wl = net.test_wlans[0]
    assert w.poll(0) is False
    assert wl.calls == [("active", True), ("pm", 0x10), ("connect", "tufty-video")]
    assert w.poll(100) is False and w.text == "joining tufty-video"
    wl.connected = True
    assert w.poll(200) is True and w.ip == "192.168.4.2"
    assert wl.calls[-1] == ("pm", 0x10)



def test_pm_constant_comes_from_the_driver(net, monkeypatch):
    assert net.PM_NONE == 0x10  # read from WLAN.PM_NONE, not a literal
    monkeypatch.delattr(FakeWLAN, "PM_NONE")
    importlib.reload(net)
    assert net.PM_NONE is None
    w = net.Wifi("x", "pw", diff)
    w.poll(0)
    assert not any(c[0] == "pm" for c in net.test_wlans[-1].calls)  # no guessed value
def test_existing_association_is_dropped_first(net):
    w = net.Wifi("tufty-video", "pw", diff)
    wl = net.test_wlans[0]
    wl._active = True
    wl.connected = True  # e.g. still on home Wi-Fi from the launcher
    assert w.poll(0) is False
    assert wl.calls[0] == ("disconnect",) and ("connect", "tufty-video") in wl.calls


def test_wrong_password_then_retry_after_delay(net):
    w = net.Wifi("x", "pw", diff)
    wl = net.test_wlans[0]
    w.poll(0)
    wl.st = -3
    w.poll(10)
    assert w.text == "wrong password" and not w.joining
    n = len(wl.calls)
    w.poll(10 + net.RETRY_MS - 1)
    assert len(wl.calls) == n
    w.poll(10 + net.RETRY_MS)
    assert wl.calls[-1] == ("connect", "x")


def test_join_timeout_disconnects(net):
    w = net.Wifi("x", "pw", diff)
    wl = net.test_wlans[0]
    w.poll(0)
    w.poll(net.JOIN_TIMEOUT_MS)
    assert w.text == "join timeout" and ("disconnect",) in wl.calls


def test_link_loss_is_reported_and_rejoined(net):
    w = net.Wifi("x", "pw", diff)
    wl = net.test_wlans[0]
    w.poll(0)
    wl.connected = True
    assert w.poll(1)
    wl.connected = False
    assert w.poll(2) is False and not w.up
    assert wl.calls[-1] == ("connect", "x")  # rejoins at once after a loss


def test_open_video_is_nonblocking_with_nodelay(net):
    s = net.open_video()
    assert s.blocking is False and (6, 1, 1) in s.opts and s.addr == ("192.168.4.1", 8081)
    assert not s.closed


def test_open_video_closes_on_real_error(net):
    real = FakeSock.connect

    def refuse(self, addr):
        raise OSError(errno.EHOSTUNREACH)
    FakeSock.connect = refuse
    try:
        with pytest.raises(OSError):
            net.open_video()
        assert net.test_socks[-1].closed
    finally:
        FakeSock.connect = real
