"""Two opaque video-statistics lines and connection status panels.

The framebuffer retains the last camera image between frames. Text is cached
at most four times per second, then blitted only over new or changed pixels.
"""

REFRESH_MS = 250
LINE_H = 12

INK = color.rgb(235, 240, 240)
MUTED = color.rgb(150, 165, 170)
BAR = color.rgb(0, 0, 0)
PANEL = color.rgb(20, 28, 34)
GOOD = color.rgb(110, 220, 160)


def canvas(width, height, background):
    """Use stock PicoVector images and the display's existing bitmap font."""
    target = image(width, height)
    target.font = screen.font
    target.pen = background
    target.clear()
    return target


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
        self.top_sprite = canvas(screen.width, LINE_H, BAR)
        self.bottom_sprite = canvas(screen.width, LINE_H, BAR)
        self._raster = (None, None)
        self._text_changed = True

    def toggle(self):
        self.visible = not self.visible
        self.t_refresh = None
        self._text_changed = True

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
        if not self.visible:
            return
        s = app.stream
        self.top = "%d.%dfps n%d d%d" % (
            self.fps10 // 10, self.fps10 % 10, s.net_ms, app.decode_ms)
        kb10 = s.frame_len * 10 // 1024
        self.bottom = "s%d g%d a%d %d.%dk" % (
            app.display_ms, app.gc_ms, s.age_us // 1000, kb10 // 10, kb10 % 10)
        lines = (self.top, self.bottom)
        if lines != self._raster:
            for sprite, text, previous in zip(
                    (self.top_sprite, self.bottom_sprite), lines, self._raster):
                if text != previous:
                    sprite.pen = BAR
                    sprite.clear()
                    sprite.pen = INK
                    sprite.text(text, 2, 0)
            self._raster = lines
            self._text_changed = True

    def draw(self, app, now, frame_changed=True):
        if not self.visible or not (frame_changed or self._text_changed):
            return
        screen.blit(self.top_sprite, 0, 0)
        screen.blit(self.bottom_sprite, 0, screen.height - LINE_H)
        self._text_changed = False

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
