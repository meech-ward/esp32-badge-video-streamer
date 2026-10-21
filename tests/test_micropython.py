"""Exercise the wire module on a Unix MicroPython binary when one is available."""
import os
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def find_micropython():
    explicit = os.environ.get("TUFTY_VIDEO_MICROPYTHON")
    if explicit is not None:
        if not os.path.isfile(explicit) or not os.access(explicit, os.X_OK):
            pytest.fail("TUFTY_VIDEO_MICROPYTHON must name an executable Unix MicroPython binary")
        return explicit
    return shutil.which("micropython")


SMOKE = r"""
import stream_protocol as P
hello = P.pack_hello(P.FS_QVGA, 6, 1)
assert hello == b"VH\x01\x01\x06\x01\x00\x00", hello
assert P.unpack_hello(hello) == (1, 6, 1)
bounded = P.pack_hello(P.FS_QVGA, 6, 1, 64)
assert bounded[-2:] == b"\x40\x00" and P.unpack_hello(bounded) == (1, 6, 1)
header = P.pack_header(P.TYPE_FRAME, 0x1234, 160, 999, 5000)
assert len(header) == 16
assert P.unpack_header(header) == (1, 0x1234, 160, 999, 5000)
for bad in (b"VS", b"VH\x01\x01\x06\x01\x81\x00"):
    try:
        if len(bad) == 8:
            P.unpack_hello(bad)
        else:
            P.unpack_header(bad)
        raise AssertionError("invalid packet accepted")
    except P.ProtocolError:
        pass
print("VIDEO_MP_OK")
"""


def test_protocol_runs_under_micropython():
    mp = find_micropython()
    if mp is None:
        pytest.skip("set TUFTY_VIDEO_MICROPYTHON to a Unix MicroPython binary")
    result = subprocess.run(
        [mp, "-c", SMOKE],
        env=dict(os.environ, MICROPYPATH=str(ROOT / "tufty/video_streamer")),
        capture_output=True, text=True, timeout=30)
    assert result.returncode == 0 and "VIDEO_MP_OK" in result.stdout, result.stdout + result.stderr
