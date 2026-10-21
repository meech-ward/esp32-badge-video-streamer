"""Client for the video-only VH/VS protocol on TCP 8081."""
import collections
import pathlib
import socket
import sys
import time

_ROOT = pathlib.Path(__file__).resolve().parent.parent
if str(_ROOT / "tufty/video_streamer") not in sys.path:
    sys.path.insert(0, str(_ROOT / "tufty/video_streamer"))

import stream_protocol as P  # noqa: E402

SIZE_NAMES = {"qqvga": P.FS_QQVGA, "qvga": P.FS_QVGA, "240": P.FS_240, "vga": P.FS_VGA}

Message = collections.namedtuple("Message", "mtype seq width payload age_us")


def parse_size(text):
    """'qqvga'/'qvga'/'240'/'vga' or a frame-size code."""
    key = str(text).lower()
    if key in SIZE_NAMES:
        return SIZE_NAMES[key]
    code = int(key)
    if code not in P.FRAME_SIZES:
        raise ValueError(f"unknown frame size {text!r}")
    return code



class VideoClient:
    def __init__(self, host, port=P.VIDEO_PORT, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        # Credits are single bytes; without NODELAY Nagle can hold one back
        # behind the previous unacknowledged credit and stall the stream.
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._hdr = bytearray(P.HEADER_SIZE)
        # Partial-message state lives on the object, not in read()'s locals: a
        # socket timeout can fire mid-header or mid-payload (a Wi-Fi stall), and
        # the caller's retry must resume at the same byte or the next "header"
        # would be parsed out of JPEG data.
        self._got = 0
        self._meta = None  # parsed header of the message being received
        self._payload = None
        self.last_rx = time.monotonic()  # last time any byte arrived

    def hello(self, framesize=P.FS_QQVGA, quality=6, credits=1):
        self.sock.sendall(P.pack_hello(framesize, quality, credits))

    def credit(self, n=1):
        if n > 0:
            self.sock.sendall(P.CREDIT * n)

    def _fill(self, buf):
        view = memoryview(buf)
        while self._got < len(buf):
            n = self.sock.recv_into(view[self._got:])  # socket.timeout keeps self._got
            if n == 0:
                raise ConnectionError("video connection closed")
            self._got += n
            self.last_rx = time.monotonic()
        self._got = 0

    def read(self):
        """Block for the next message. Raises ProtocolError, ConnectionError, socket.timeout.

        After socket.timeout, calling read() again continues the same message.
        """
        if self._meta is None:
            self._fill(self._hdr)
            self._meta = P.unpack_header(self._hdr)
            self._payload = bytearray(self._meta[3])
        self._fill(self._payload)
        (mtype, seq, width, _len, age), payload = self._meta, self._payload
        self._meta = self._payload = None
        return Message(mtype, seq, width, bytes(payload), age)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
