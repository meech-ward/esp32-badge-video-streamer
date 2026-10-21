"""Stock Badgeware camera viewer. B toggles statistics; HOME returns to the menu."""
import gc
import time

# No VSYNC: otherwise each update rounds up to a display-refresh interval.
# Keep the last image between updates so waiting for TCP needs no redraw.
badge.mode(LORES)
badge.default_clear = None

import stream_protocol as P  # noqa: E402
import stream as _stream  # noqa: E402
import overlay as _overlay  # noqa: E402
import link as _link  # noqa: E402
import telemetry as _telemetry  # noqa: E402
import net  # noqa: E402

FRAMESIZE = P.FS_QQVGA
QUALITY = 6
CREDITS = 1
FRAME_CAP = 32 * 1024
BACKOFF_MAX_MS = 1000
SPIN_MS = 12
# Automatic GC stays enabled; amortise explicit full collections instead of
# inserting one into the display path almost every second.
GC_EVERY = 300
GC_IDLE_MS = 1000
SERIAL_DIAGNOSTICS = False

ticks_ms = time.ticks_ms
ticks_diff = time.ticks_diff


def _load_wifi_config():
    # Use the app's absolute path so configuration always belongs to this app.
    try:
        return __import__("/system/apps/video_streamer/wifi_config")
    except ImportError:
        raise ImportError("Copy wifi_config.example.py to video_streamer/wifi_config.py")


class App:
    def __init__(self):
        now = ticks_ms()
        self.overlay = _overlay.Overlay(ticks_diff)
        self.log = _telemetry.Logger(ticks_ms) if SERIAL_DIAGNOSTICS else None
        self.stats = _telemetry.Stats(ticks_diff)
        cfg = _load_wifi_config()
        self.wifi = net.Wifi(cfg.SSID, cfg.PASSWORD, ticks_diff, log=self.log)
        self.stream = _stream.Stream(
            net.open_video, ticks_ms, ticks_diff, FRAMESIZE, QUALITY, CREDITS,
            FRAME_CAP, spin_ms=SPIN_MS, backoff_max_ms=BACKOFF_MAX_MS, log=self.log)
        self.link = _link.Link(self.wifi, self.stream, ticks_diff)
        self.shown = 0
        self.shown_connect = 0
        self.decode_ms = 0
        self.decode_errors = 0
        self.display_ms = 0
        self.gc_ms = 0
        self.gc_max_ms = 0
        self.gc_count = 0
        self.t_gc = now
        self.t_end = None
        self.repaint = False
        self.mode_text = P.ESP_ADDR

    def collect(self, now):
        t = ticks_ms()
        gc.collect()
        self.gc_count += 1
        self.gc_ms = ticks_diff(ticks_ms(), t)
        if self.gc_ms > self.gc_max_ms:
            self.gc_max_ms = self.gc_ms
        self.t_gc = now

    def decode(self):
        t = ticks_ms()
        try:
            # Decode straight from the preallocated receive buffer into screen RAM.
            screen.load_into(self.stream.frame_view())
            self.repaint = False
            return True
        except (ValueError, OSError, MemoryError):
            self.decode_errors += 1
            self.stream.frame_valid = False
            return False
        finally:
            self.decode_ms = ticks_diff(ticks_ms(), t)

    def update(self):
        now = ticks_ms()
        if self.t_end is not None:
            # Includes the managed loop's display.update() and input poll.
            self.display_ms = ticks_diff(now, self.t_end)
        if badge.pressed(BUTTON_B):
            self.overlay.toggle()
            self.repaint = True

        s = self.stream
        frame_changed = False
        if self.link.poll(now):
            if s.poll(now):
                # A rejected JPEG can already have changed framebuffer pixels.
                frame_changed = True
                drew = self.decode()
                # Pixels are already in screen RAM: return credit before display
                # presentation so the ESP can prepare the next frame immediately.
                s.frame_drawn()
                if drew:
                    self.shown += 1
                    self.shown_connect = s.connects
                    if self.shown % GC_EVERY == 0:
                        self.collect(now)
        elif s.state != _stream.IDLE:
            s.close("wifi down", backoff=False)

        # Toggling HUD off restores the retained JPEG without awarding another
        # credit. If the next JPEG has begun overwriting it, wait for that frame.
        if self.repaint and s.frame_valid and self.shown:
            frame_changed = True
            self.decode()

        if not s.connected() or self.shown_connect != s.connects:
            if ticks_diff(now, self.t_gc) >= GC_IDLE_MS:
                self.collect(now)
            self.draw_status(now)
        else:
            self.overlay.refresh(now, self)
            self.overlay.draw(self, now, frame_changed=frame_changed)
        if SERIAL_DIAGNOSTICS:
            line = self.stats.line(now, self)
            if line is not None:
                print(line)
        self.t_end = ticks_ms()
        return None

    def draw_status(self, now):
        s = self.stream
        if not self.wifi.up:
            title, detail = "Joining Wi-Fi", self.wifi.text
        elif s.state == _stream.CONNECTING:
            title, detail = "Connecting", self.mode_text
        elif s.state == _stream.BACKOFF:
            title, detail = "Retrying", s.reason
        elif s.connected():
            title, detail = "Waiting for frames", self.mode_text
        else:
            title, detail = "Starting", self.mode_text
        self.overlay.draw_status(self, now, title, detail)

    def stop(self):
        self.stream.close("exit", backoff=False)
        self.wifi.stop()


app = App()


def update():
    return app.update()


def on_exit():
    app.stop()


def debug_state():
    """A compact snapshot for offline emulator checks and serial diagnostics."""
    s = app.stream
    return {"state": _stream.STATE_NAMES[s.state], "frames": s.frames,
            "shown": app.shown, "statuses": s.statuses,
            "decode_errors": app.decode_errors, "connects": s.connects,
            "errors": s.errors, "reason": s.reason, "seq": s.seq,
            "width": s.width, "frame_len": s.frame_len,
            "hud": app.overlay.visible, "fps10": app.overlay.fps10,
            "decode_ms": app.decode_ms, "net_ms": s.net_ms,
            "display_ms": app.display_ms, "gc_ms": app.gc_ms,
            "gc_max_ms": app.gc_max_ms, "gc_count": app.gc_count,
            "top": app.overlay.top, "bottom": app.overlay.bottom,
            "mem_free": gc.mem_free()}


run(update)
