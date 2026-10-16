"""App lifecycle and frame/HUD ordering with injected stock-runtime interfaces."""
import builtins
import gc
import pathlib
import runpy
import sys
import time
import types

import pytest

import stream_protocol as P

APP = pathlib.Path(__file__).resolve().parents[1] / "tufty" / "video_streamer"


class Socket:
    def __init__(self):
        self.rx = bytearray()
        self.sent = bytearray()
        self.closed = False

    def send(self, data):
        self.sent.extend(data)
        return len(data)

    def readinto(self, buffer):
        if not self.rx:
            return None
        count = min(len(buffer), len(self.rx))
        buffer[:count] = self.rx[:count]
        del self.rx[:count]
        return count

    def feed(self, jpeg=b"jpeg", seq=1):
        self.rx.extend(P.pack_header(P.TYPE_FRAME, seq, 160, len(jpeg)) + jpeg)

    def close(self):
        self.closed = True


class Screen:
    width, height = 160, 120

    def __init__(self):
        self.ops = []
        self.pen = None

    def load_into(self, data):
        self.ops.append(("load", bytes(data)))
        if bytes(data) == b"bad":
            raise ValueError("bad JPEG")

    def clear(self):
        self.ops.append(("clear",))

    def rectangle(self, *args):
        self.ops.append(("rect",) + args)

    def text(self, text, *args):
        self.ops.append(("text", text) + args)


class Wifi:
    def __init__(self, *args, **kwargs):
        self.up = True
        self.text = "test Wi-Fi"
        self.stopped = False

    def poll(self, now):
        return self.up

    def reset(self, now, reason):
        self.up = False

    def stop(self):
        self.stopped = True
        self.up = False


@pytest.fixture
def rig(monkeypatch):
    clock = types.SimpleNamespace(t=0)
    screen = Screen()
    presses = set()
    modes = []
    badge = types.SimpleNamespace(mode=modes.append, pressed=presses.__contains__)
    sockets = []

    def open_video():
        sock = Socket()
        sockets.append(sock)
        return sock

    monkeypatch.setitem(sys.modules, "net", types.SimpleNamespace(Wifi=Wifi, open_video=open_video))
    monkeypatch.setitem(sys.modules, "/system/apps/video_streamer/wifi_config",
                        types.SimpleNamespace(SSID="test", PASSWORD="placeholder"))
    monkeypatch.setattr(builtins, "screen", screen, raising=False)
    monkeypatch.setattr(builtins, "color", types.SimpleNamespace(rgb=lambda *rgb: rgb), raising=False)
    monkeypatch.setattr(builtins, "badge", badge, raising=False)
    monkeypatch.setattr(builtins, "LORES", 1, raising=False)
    monkeypatch.setattr(builtins, "BUTTON_B", "B", raising=False)
    monkeypatch.setattr(builtins, "run", lambda update: None, raising=False)
    monkeypatch.setattr(time, "ticks_ms", lambda: clock.t, raising=False)
    monkeypatch.setattr(time, "ticks_diff", lambda a, b: a - b, raising=False)
    monkeypatch.setattr(gc, "mem_free", lambda: 123456, raising=False)
    namespace = runpy.run_path(str(APP / "__init__.py"))
    app = namespace["app"]
    app.stream.spin_ms = 0  # deterministic fake clock does not advance while reading
    app.update()  # open TCP and show initial waiting panel
    assert modes == [1] and badge.default_clear is None
    return types.SimpleNamespace(app=app, clock=clock, screen=screen, presses=presses,
                                 sockets=sockets, namespace=namespace)


def test_decode_returns_credit_before_presentation_and_hide_restores_camera(rig):
    app, sock = rig.app, rig.sockets[0]
    sock.feed()
    app.update()
    assert app.shown == 1
    assert bytes(sock.sent) == P.pack_hello(P.FS_QQVGA, 6, 1) + P.CREDIT
    assert rig.screen.ops[-4][0] == "rect"  # decoder ran before HUD drawing
    rig.screen.ops.clear()
    rig.presses.add("B")
    app.update()
    assert not app.overlay.visible and rig.screen.ops == [("load", b"jpeg")]
    assert bytes(sock.sent[P.HELLO_SIZE:]) == P.CREDIT  # redraw is not another frame
    assert app.shown == 1
    rig.presses.clear()
    rig.screen.ops.clear()
    app.update()
    assert rig.screen.ops == []


def test_hud_toggle_waits_for_partial_frame_and_never_decodes_corrupted_cache(rig):
    app, sock = rig.app, rig.sockets[0]
    sock.feed()
    app.update()
    sock.rx.extend(P.pack_header(P.TYPE_FRAME, 2, 160, 4) + b"ne")
    app.update()
    assert not app.stream.frame_valid
    rig.screen.ops.clear()
    rig.presses.add("B")
    app.update()
    assert rig.screen.ops == [] and app.repaint
    rig.presses.clear()
    sock.rx.extend(b"xt")
    app.update()
    assert rig.screen.ops == [("load", b"next")]
    assert app.shown == 2 and not app.repaint
    assert bytes(sock.sent[P.HELLO_SIZE:]) == P.CREDIT * 2


def test_hidden_hud_retains_retry_status_until_a_good_frame_on_new_connection(rig):
    app = rig.app
    rig.sockets[0].feed()
    app.update()
    rig.presses.add("B")
    app.update()
    rig.presses.clear()
    app.stream.close("test disconnect", backoff=False)
    rig.screen.ops.clear()
    app.update()
    assert ("text", "Waiting for frames", 8, 36) in rig.screen.ops
    # Invalid JPEG must not erase the waiting panel or strand the stream's credit.
    rig.screen.ops.clear()
    rig.sockets[1].feed(b"bad")
    app.update()
    assert app.decode_errors == 1 and app.shown == 1
    assert any(op[:2] == ("text", "Waiting for frames") for op in rig.screen.ops)
    rig.screen.ops.clear()
    rig.sockets[1].feed(b"new", 2)
    app.update()
    assert rig.screen.ops == [("load", b"new")]
    assert app.shown == 2


def test_home_cleanup_closes_tcp_and_disconnects_wifi(rig):
    rig.namespace["on_exit"]()
    assert rig.sockets[0].closed and rig.app.wifi.stopped
    assert rig.namespace["debug_state"]()["state"] == "idle"
