"""Two opaque video-statistics lines and connection status panels.

The framebuffer retains the last camera image between frames. Text is rebuilt
at most four times per second; repeated draws remain opaque and idempotent.
"""

REFRESH_MS = 250
LINE_H = 12

INK = color.rgb(235, 240, 240)
MUTED = color.rgb(150, 165, 170)
BAR = color.rgb(0, 0, 0)
PANEL = color.rgb(20, 28, 34)
GOOD = color.rgb(110, 220, 160)


class Overlay:
    def __init__(self, ticks_diff):
        self.ticks_diff = ticks_diff
        self.visible = True
        self.top = ""
        self.bottom = ""
        self.t_refresh = None
        self.t_fps = None
        self.frames_at = 0
        self.fps10 = 0

    def toggle(self):
        self.visible = not self.visible
        self.t_refresh = None

    def refresh(self, now, app):
        if (self.t_refresh is not None
                and self.ticks_diff(now, self.t_refresh) < REFRESH_MS):
            return
        if self.t_fps is None:
            self.t_fps = now
            self.frames_at = app.shown
        dt = self.ticks_diff(now, self.t_fps)
        if dt >= 1000:
            self.fps10 = (app.shown - self.frames_at) * 10000 // dt
            self.frames_at = app.shown
            self.t_fps = now
        self.t_refresh = now
        s = app.stream
        self.top = "%d.%dfps n%d d%d" % (
            self.fps10 // 10, self.fps10 % 10, s.net_ms, app.decode_ms)
        kb10 = s.frame_len * 10 // 1024
        self.bottom = "s%d g%d a%d %d.%dk" % (
            app.display_ms, app.gc_ms, s.age_us // 1000, kb10 // 10, kb10 % 10)

    def draw(self, app, now):
        if not self.visible:
            return
        screen.pen = BAR
        screen.rectangle(0, 0, screen.width, LINE_H)
        screen.pen = INK
        screen.text(self.top, 2, 0)
        screen.pen = BAR
        screen.rectangle(0, screen.height - LINE_H, screen.width, LINE_H)
        screen.pen = INK
        screen.text(self.bottom, 2, screen.height - LINE_H)

    def draw_status(self, app, now, title, detail):
        """Status stays visible with the HUD off; hide it on the next good frame."""
        w = screen.width
        h = screen.height
        y = h // 2 - 2 * LINE_H
        screen.pen = PANEL
        if app.shown == 0:
            screen.clear()
        else:
            screen.rectangle(4, y - 4, w - 8, 3 * LINE_H + 8)
        screen.pen = GOOD
        screen.text(title, 8, y)
        screen.pen = INK
        screen.text(detail, 8, y + LINE_H)
        screen.pen = MUTED
        screen.text("B: HUD | HOME: exit", 8, y + 2 * LINE_H)
