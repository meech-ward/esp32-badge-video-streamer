"""Video HUD behavior and status visibility on a recording Badgeware screen."""
import builtins
import types

import pytest

from telemetry import Logger, Stats


class FakeScreen:
    width, height = 160, 120

    def __init__(self):
        self.pen = None
        self.ops = []

    def clear(self):
        self.ops.append(("clear", self.pen))

    def rectangle(self, x, y, width, height):
        self.ops.append(("rect", self.pen, x, y, width, height))

    def text(self, text, x, y):
        self.ops.append(("text", self.pen, text, x, y))


@pytest.fixture
def scr(monkeypatch):
    scr = FakeScreen()
    monkeypatch.setattr(builtins, "screen", scr, raising=False)
    monkeypatch.setattr(builtins, "color", types.SimpleNamespace(rgb=lambda *rgb: rgb), raising=False)
    return scr


@pytest.fixture
def overlay(scr):
    # Stock Badgeware installs these builtins before importing rendering code.
    # Fixtures restore them after each test instead of polluting collection.
    from overlay import Overlay
    return Overlay(lambda a, b: a - b)


def app_stub():
    return types.SimpleNamespace(
        stream=types.SimpleNamespace(frames=0, net_ms=7, frame_len=3584, age_us=4200),
        shown=1, decode_ms=9, display_ms=11, gc_ms=2)


def test_hud_starts_visible_and_b_toggles_all_stats(scr, overlay):
    ov = overlay
    app = app_stub()
    ov.refresh(0, app)
    ov.draw(app, 0)
    assert ov.visible
    assert [op[2] for op in scr.ops if op[0] == "text"] == [
        "0.0fps n7 d9", "s11 g2 a4 3.5k"]
    assert [op[2:] for op in scr.ops if op[0] == "rect"] == [
        (0, 0, 160, 12), (0, 108, 160, 12)]
    ov.toggle()
    scr.ops.clear()
    ov.draw(app, 1)
    assert not ov.visible and scr.ops == []
    ov.toggle()
    ov.refresh(2, app)
    ov.draw(app, 2)
    assert ov.visible and len(scr.ops) == 4


def test_frame_rate_counts_presented_frames_and_text_refresh_is_bounded(overlay):
    ov = overlay
    app = app_stub()
    ov.refresh(0, app)
    app.stream.net_ms = 20
    app.stream.frames = 999  # a received frame that failed decode is not presented
    ov.refresh(100, app)
    assert ov.top == "0.0fps n7 d9"
    app.shown += 36
    ov.refresh(1000, app)
    assert ov.top == "36.0fps n20 d9"


@pytest.mark.parametrize("shown", [0, 1])
def test_status_remains_visible_with_hud_off(scr, shown, overlay):
    ov = overlay
    ov.toggle()
    app = app_stub()
    app.shown = shown
    ov.draw_status(app, 0, "Retrying", "link dead")
    assert [op[2] for op in scr.ops if op[0] == "text"] == [
        "Retrying", "link dead", "B: HUD | HOME: exit"]
    assert scr.ops[0][0] == ("clear" if shown == 0 else "rect")
    assert all(op[4] < scr.height for op in scr.ops if op[0] == "text")


def test_console_stats_are_video_only_and_infrequent():
    st = Stats(lambda a, b: a - b)
    app = app_stub()
    assert st.line(0, app) is None
    app.shown += 180
    assert st.line(4999, app) is None
    assert st.line(5000, app) == "video: fps 36.0 n 7 d 9 s 11 g 2 kb 3.5 age 4"
    assert st.line(5001, app) is None
    lines = []
    Logger(lambda: 1234, lines.append)("wifi up")
    assert lines == ["video: wifi up @1234"]
