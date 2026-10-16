"""Test-only camera socket and WLAN substitute for the graphics emulator.

Never installed on a badge. Real stream parsing and JPEG decoding still run.
"""
import errno
import json
import sys
import time

import stream_protocol as P

CHUNKS = (700, 1460, 333, 2048, 24, 6400)

class FakeVideoSocket:
    def __init__(self, frames, ticks_ms, ticks_diff, fps=25):
        self.frames = frames  # {framesize: [jpeg bytes, ...]}
        self.ticks_ms = ticks_ms
        self.ticks_diff = ticks_diff
        self.period = 1000 // fps
        self.inbox = bytearray()
        self.out = bytearray()
        self.off = 0
        self.hello = None
        self.credits = 0
        self.seq = 0
        self.status_seq = 0
        self.chunk = 0
        self.connect_polls = 2  # first sends report EAGAIN, like a real connect
        self.t_frame = None
        self.t_status = None
        self.closed = False
        self.sent_frames = 0

    def send(self, buf):
        if self.closed:
            raise OSError(errno.EBADF)
        if self.connect_polls:
            self.connect_polls -= 1
            raise OSError(errno.EAGAIN)
        self.inbox.extend(buf)
        self._consume()
        return len(buf)

    def _consume(self):
        if self.hello is None and len(self.inbox) >= P.HELLO_SIZE:
            self.hello = P.unpack_hello(bytes(self.inbox[:P.HELLO_SIZE]))
            self.inbox = self.inbox[P.HELLO_SIZE:]
            self.credits = self.hello[2]
        if self.hello is not None:
            for b in self.inbox:
                if b != P.CREDIT[0]:
                    raise OSError(errno.ECONNRESET)  # the ESP closes on a bad credit byte
                self.credits = min(P.MAX_CREDITS, self.credits + 1)
            self.inbox = bytearray()

    def _produce(self):
        now = self.ticks_ms()
        if self.hello is None:
            return
        if self.t_status is None or self.ticks_diff(now, self.t_status) >= 1000:
            self.t_status = now
            self.status_seq += 1
            body = json.dumps({"fps": 25, "cam_fps": 25,
                               "sensor": "TEST", "fs": self.hello[0], "q": self.hello[1]}).encode()
            self.out.extend(P.pack_header(P.TYPE_STATUS, self.status_seq, 0, len(body), 0))
            self.out.extend(body)
        if self.credits and (self.t_frame is None or self.ticks_diff(now, self.t_frame) >= self.period):
            self.t_frame = now
            self.credits -= 1
            fs = self.hello[0]
            pool = self.frames[fs]
            jpeg = pool[self.seq % len(pool)]
            self.seq += 1
            self.sent_frames += 1
            w = P.FRAME_SIZES[fs][0]
            self.out.extend(P.pack_header(P.TYPE_FRAME, self.seq, w, len(jpeg), 4000))
            self.out.extend(jpeg)

    def readinto(self, buf):
        if self.closed:
            raise OSError(errno.EBADF)
        if self.off >= len(self.out):
            self.out = bytearray()
            self.off = 0
            self._produce()
            if not self.out:
                return None
        n = CHUNKS[self.chunk % len(CHUNKS)]
        self.chunk += 1
        left = len(self.out) - self.off
        n = min(n, left, len(buf))
        buf[:n] = self.out[self.off:self.off + n]
        self.off += n
        return n

    def close(self):
        self.closed = True


class Wifi:
    def __init__(self, ssid, password, ticks_diff, log=None):
        self.ssid = ssid
        self.up = False
        self.text = "test camera"
        self.resets = 0
        self.cycles = 0

    def poll(self, now):
        self.up = True
        return True

    def reset(self, now, reason):
        self.up = False
        self.resets += 1

    def stop(self):
        self.up = False


def install():
    with open("/fixtures/frame_160x120.jpg", "rb") as source:
        frames = {P.FS_QQVGA: [source.read()]}
    module = sys.modules[__name__]
    module.Wifi = Wifi
    module.open_video = lambda: FakeVideoSocket(frames, time.ticks_ms, time.ticks_diff)
    sys.modules["net"] = module
