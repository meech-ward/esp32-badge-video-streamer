"""Exercise the C/C++ endpoint helper with marker grammar and stale DMA tails."""
from io import BytesIO
import os
from pathlib import Path
import shutil
import subprocess

from PIL import Image
import pytest

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "esp/components/camera_source/include"


def segment(marker, payload):
    return bytes((0xff, marker)) + (len(payload) + 2).to_bytes(2, "big") + payload


def frame(height=8):
    return segment(0xc0, bytes((8, height >> 8, height & 255, 0, 8, 1, 1, 0x11, 0)))


SCAN = segment(0xda, b"\x01\x01\x00\x00\x3f\x00")
SOI, EOI = b"\xff\xd8", b"\xff\xd9"


def fixtures():
    output = BytesIO()
    Image.new("L", (8, 8), 127).save(output, format="JPEG", quality=70)
    jpeg = output.getvalue()  # genuinely decodable, with no private photograph
    simple = SOI + frame() + SCAN + b"\x12\x34" + EOI
    metadata = segment(0xe1, b"prefix" + EOI + jpeg + b"suffix")
    metadata += segment(0xfe, EOI) + segment(0xdb, b"\x00" + EOI)
    embedded = SOI + metadata + frame() + SCAN + b"\x12" + EOI
    stuffed = SOI + frame() + SCAN + b"\xff\x00\xd9\x12"
    stuffed += b"".join(bytes((0xff, marker)) + b"\x23" for marker in range(0xd0, 0xd8))
    stuffed += b"\xff\xff\xd9"
    filled = b"\xff\xff\xd8\xff" + frame() + b"\xff" + SCAN + b"\x12\xff\xff\xd9"
    multiple = SOI + frame() + SCAN + b"\x12" + segment(0xc4, EOI)
    multiple += SCAN + b"\x34" + EOI
    dnl = SOI + frame(0) + SCAN + b"\x12" + segment(0xdc, b"\x00\x08")
    dnl_extended = SOI + frame(0) + SCAN + b"\x12" + segment(0xdc, b"\x00\x08\x00")
    tem = SOI + b"\xff\x01" + frame() + SCAN + b"\x12\xff\x01\x34" + EOI

    cases = []

    def good(name, value, tail=b""):
        cases.append((name, value + tail, len(value)))

    def bad(name, value):
        cases.append((name, value, 0))

    for name, value in (("real_jpeg", jpeg), ("metadata_thumbnail", embedded),
                        ("stuff_restart_fill", stuffed), ("marker_fill", filled),
                        ("multiple_scans", multiple), ("dnl_eoi", dnl + EOI),
                        ("dnl_next_scan", dnl + SCAN + b"\x34" + EOI),
                        ("extended_dnl", dnl_extended + EOI), ("tem", tem)):
        good(name, value)
    good("trailing_second_jpeg", embedded, b"stale" + jpeg)

    # Reproduce the measured first-EOI/captured lengths without storing a user's
    # photo: the tail is literally the old larger JPEG's unchanged suffix.
    for current_len in (5650, 5689):
        def padded(size):
            payload = b"embedded" + EOI + b"x" * (size - len(jpeg) - 4 - 10)
            result = SOI + segment(0xe2, payload) + jpeg[2:]
            assert len(result) == size
            return result

        current, old = padded(current_len), padded(7561)
        captured = current + old[current_len:]
        assert captured[current_len:] == old[current_len:] and captured.endswith(EOI)
        good(f"captured_{current_len}_7561", current, captured[current_len:])

    for name, value in (("empty", b""), ("bare_soi_eoi", SOI + EOI),
                        ("leading_junk", b"x" + simple), ("eoi_before_scan", SOI + frame() + EOI),
                        ("sos_before_frame", SOI + SCAN + b"x" + EOI),
                        ("nested_soi", SOI + frame() + SCAN + SOI + EOI),
                        ("restart_outside", SOI + b"\xff\xd0" + frame() + SCAN + EOI),
                        ("stuff_outside", SOI + b"\xff\x00" + frame() + SCAN + EOI),
                        ("filled_stuff", SOI + frame() + SCAN + b"\xff\xff\x00" + EOI),
                        ("unknown_marker", SOI + b"\xff\x80\x00\x02" + simple[2:]),
                        ("reserved_extension", SOI + segment(0xf0, b"") + simple[2:]),
                        ("zero_segment", SOI + b"\xff\xe0\x00\x00" + simple[2:]),
                        ("one_segment", SOI + b"\xff\xe0\x00\x01" + simple[2:]),
                        ("overrun_segment", SOI + b"\xff\xe0\xff\xff" + simple[2:]),
                        ("short_frame", SOI + segment(0xc0, b"\x08") + SCAN + EOI),
                        ("bad_frame_count", SOI + segment(0xc0, b"\x08\x00\x08\x00\x08\x02\x01\x11\x00") + SCAN + EOI),
                        ("short_scan", SOI + frame() + segment(0xda, b"\x01") + EOI),
                        ("zero_scan_count", SOI + frame() + segment(0xda, b"\x00\x00\x3f\x00") + EOI),
                        ("unresolved_height", SOI + frame(0) + SCAN + b"x" + EOI),
                        ("dnl_before_scan", SOI + frame(0) + segment(0xdc, b"\x00\x08") + SCAN + EOI),
                        ("dnl_zero_height", SOI + frame(0) + SCAN + segment(0xdc, b"\x00\x00") + EOI),
                        ("dnl_after_second_scan", SOI + frame(0) + SCAN + b"x" + SCAN + b"y" + segment(0xdc, b"\x00\x08") + EOI),
                        ("dnl_after_metadata", SOI + frame(0) + SCAN + b"x" + segment(0xe0, b"") + segment(0xdc, b"\x00\x08") + EOI),
                        ("dnl_after_table", SOI + frame(0) + SCAN + b"x" + segment(0xc4, b"") + segment(0xdc, b"\x00\x08") + EOI),
                        ("repeated_dnl", dnl + segment(0xdc, b"\x00\x08") + EOI),
                        ("entropy_after_dnl", dnl + b"\x12\x34" + EOI),
                        ("no_eoi", simple[:-2])):
        bad(name, value)
    return cases, embedded


@pytest.mark.parametrize("language", ["c", "c++"])
def test_endpoint_grammar_and_all_truncated_prefixes(tmp_path, language):
    if language == "c":
        compiler = os.environ.get("HOST_CC") or shutil.which("clang") or shutil.which("gcc")
        standard, suffix = "c11", "c"
    else:
        compiler = os.environ.get("HOST_CXX") or shutil.which("clang++") or shutil.which("g++")
        standard, suffix = "c++20", "cpp"
    if not compiler:
        pytest.skip("host compiler is required")
    cases, prefix_fixture = fixtures()
    source = '#include "jpeg_endpoint.h"\n#include <assert.h>\n#include <stdio.h>\n#include <stdlib.h>\n'
    for index, (_, value, _) in enumerate(cases):
        source += f"static const uint8_t data_{index}[] = {{{','.join(map(str, value)) or '0'}}};\n"
    source += "int main(void) {\nassert(streamer_jpeg_endpoint(NULL, 100) == 0);\n"
    for index, (name, value, expected) in enumerate(cases):
        source += f"assert(streamer_jpeg_endpoint(data_{index}, {len(value)}) == {expected}); /* {name} */\n"
    index = next(i for i, (_, value, _) in enumerate(cases) if value == prefix_fixture)
    source += f"""for (size_t n = 0; n < {len(prefix_fixture)}; ++n) {{
        uint8_t *prefix = (uint8_t *)malloc(n ? n : 1);
        assert(prefix); memcpy(prefix, data_{index}, n);
        assert(streamer_jpeg_endpoint(prefix, n) == 0); free(prefix);
    }}
"""
    source += 'puts("JPEG endpoint: metadata/stale tails, scans, stuffing, restarts, DNL and truncation OK");\n}\n'
    path, binary = tmp_path / f"endpoint.{suffix}", tmp_path / "endpoint"
    path.write_text(source)
    result = subprocess.run([compiler, f"-std={standard}", "-O1", "-Wall", "-Wextra", "-Werror",
                             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(INCLUDE),
                             str(path), "-o", str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "metadata/stale tails" in run.stdout
