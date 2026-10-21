"""Canonical video protocol v1 for the stock Badgeware viewer and host tools.

See docs/protocol.md; the ESP firmware has a matching C++ implementation.
"""
import struct

VERSION = 1
VIDEO_PORT = 8081
ESP_ADDR = "192.168.4.1"
HELLO_MAGIC = b"VH"
HEADER_MAGIC = b"VS"
CREDIT = b"C"
HELLO_FMT = "<2sBBBBH"
HEADER_FMT = "<2sBBHHII"
HELLO_SIZE = 8
HEADER_SIZE = 16
TYPE_FRAME = 1
TYPE_STATUS = 2
FS_QQVGA = 0
FS_QVGA = 1
FS_240 = 2
FS_VGA = 3
FRAME_SIZES = {0: (160, 120), 1: (320, 240), 2: (240, 240), 3: (640, 480)}
MAX_CREDITS = 4
MAX_PAYLOAD = 128 * 1024
STATUS_MAX = 512


class ProtocolError(ValueError):
    pass


def pack_hello(framesize=FS_QQVGA, quality=6, credits=1, max_frame_kib=0):
    if framesize not in FRAME_SIZES:
        raise ProtocolError("framesize")
    if not 4 <= quality <= 63:
        raise ProtocolError("quality")
    if not 1 <= credits <= MAX_CREDITS:
        raise ProtocolError("credits")
    if not 0 <= max_frame_kib <= MAX_PAYLOAD // 1024:
        raise ProtocolError("frame limit")
    return struct.pack(HELLO_FMT, HELLO_MAGIC, VERSION, framesize, quality, credits, max_frame_kib)


def unpack_hello(buf):
    if len(buf) != HELLO_SIZE:
        raise ProtocolError("hello size")
    magic, version, fs, quality, credits, max_frame_kib = struct.unpack(HELLO_FMT, buf)
    if magic != HELLO_MAGIC or version != VERSION:
        raise ProtocolError("hello magic/version")
    if (fs not in FRAME_SIZES or not 4 <= quality <= 63
            or not 1 <= credits <= MAX_CREDITS or max_frame_kib > MAX_PAYLOAD // 1024):
        raise ProtocolError("hello field")
    return fs, quality, credits


def pack_header(mtype, seq, width, length, age_us=0):
    return struct.pack(HEADER_FMT, HEADER_MAGIC, VERSION, mtype, seq & 0xFFFF,
                       (width // 8) & 0xFFFF, length, age_us & 0xFFFFFFFF)


def unpack_header(buf):
    """Return (type, sequence, width in pixels, payload length, age in us)."""
    if len(buf) < HEADER_SIZE:
        raise ProtocolError("header size")
    magic, version, mtype, seq, width8, length, age = struct.unpack_from(HEADER_FMT, buf, 0)
    if magic != HEADER_MAGIC or version != VERSION:
        raise ProtocolError("header magic/version")
    if mtype not in (TYPE_FRAME, TYPE_STATUS):
        raise ProtocolError("header type")
    if length > MAX_PAYLOAD or (mtype == TYPE_STATUS and length > STATUS_MAX):
        raise ProtocolError("header len")
    return mtype, seq, width8 * 8, length, age
