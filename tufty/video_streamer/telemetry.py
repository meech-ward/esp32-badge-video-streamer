"""Infrequent console statistics, independent of rendering and networking."""

STATS_MS = 5000


class Logger:
    def __init__(self, ticks_ms, out=print):
        self.ticks_ms = ticks_ms
        self.out = out

    def __call__(self, msg):
        self.out("video: %s @%d" % (msg, self.ticks_ms()))


class Stats:
    def __init__(self, ticks_diff, period_ms=STATS_MS):
        self.ticks_diff = ticks_diff
        self.period_ms = period_ms
        self.t = None
        self.frames_at = 0

    def line(self, now, app):
        """A console line every five seconds, avoiding per-frame formatting."""
        if self.t is None:
            self.t = now
            self.frames_at = app.shown
            return None
        dt = self.ticks_diff(now, self.t)
        if dt < self.period_ms:
            return None
        fps10 = (app.shown - self.frames_at) * 10000 // dt
        self.t = now
        self.frames_at = app.shown
        s = app.stream
        kb10 = s.frame_len * 10 // 1024
        return "video: fps %d.%d n %d d %d s %d g %d kb %d.%d age %d" % (
            fps10 // 10, fps10 % 10, s.net_ms, app.decode_ms,
            app.display_ms, app.gc_ms, kb10 // 10, kb10 % 10, s.age_us // 1000)
