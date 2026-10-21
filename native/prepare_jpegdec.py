#!/usr/bin/env python3
"""Stage optional JPEGDEC DSP/SRAM profiles without modifying its source checkout."""
import argparse
import pathlib
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("--dsp", action="store_true", help="fix the packed RGB565 DSP conversion")
parser.add_argument("--sram", action="store_true", help="place hot code/tables in Pico startup-copied SRAM")
args = parser.parse_args()
if not (args.dsp or args.sram):
    parser.error("select --dsp, --sram or both")
if args.source.resolve() == args.output.resolve():
    parser.error("output must be a separate build directory, not the source checkout")
args.output.mkdir(parents=True, exist_ok=True)
source = (args.source / "jpeg.inl").read_text()
# Only patch the pinned upstream expressions. A changed dependency must be
# reviewed rather than silently receiving an incomplete conversion fix.
replacements = []
if args.dsp:
    replacements.extend((
        ("-1409 | (-2925 << 16)",
         "static_cast<uint16_t>(-1409) | (static_cast<uint32_t>(static_cast<uint16_t>(-2925)) << 16)", 2),
        ("ulPixel1 |= (ulPixel2 << 16);",
         "ulPixel1 = (ulPixel1 & 0xffffu) | (ulPixel2 << 16);", 1),
        ("__USAT16(ulTmp | (ulTmp2 << 16), 5)",
         "__USAT16((ulTmp & 0xffffu) | (ulTmp2 << 16), 5)", 2),
    ))
if args.sram:
    # Decoder scratch, stacks and ownership are unchanged. Only these hot
    # functions and small read-only lookup tables move out of shared XIP.
    for result, name, count in (
        ("int", "DecodeJPEG", 2), ("int", "JPEGDecodeMCU", 1),
        ("void", "JPEGIDCT", 1), ("int32_t", "readRAM", 2),
        ("int", "JPEGFilter", 1), ("void", "JPEGGetMoreData", 2),
        ("void", "JPEGPixelLE", 1), ("void", "JPEGPixelBE", 1),
        ("void", "JPEGPixel2LE", 1), ("void", "JPEGPixel2BE", 1),
        ("void", "JPEGPutMCU11", 1), ("void", "JPEGPutMCU22", 1),
        ("void", "JPEGPutMCU12", 1), ("void", "JPEGPutMCU21", 1),
        ("void", "JPEGPutMCUGray", 1),
    ):
        old = "static " + result + " " + name + "("
        new = "static " + result + " JPEG_HOT_CODE " + name + "("
        replacements.append((old, new, count))
    for kind, name in (
        ("unsigned char", "cZigZag"), ("unsigned char", "cZigZag2"),
        ("int", "iScaleBits"), ("uint8_t", "ucRangeTable"),
        ("uint16_t", "usGrayTo565"), ("uint16_t", "usRangeTableR"),
        ("uint16_t", "usRangeTableG"), ("uint16_t", "usRangeTableB"),
    ):
        old = "static const " + kind + " " + name + "["
        new = "static const " + kind + " JPEG_HOT_DATA " + name + "["
        replacements.append((old, new, 1))
for old, new, expected in replacements:
    if source.count(old) != expected:
        parser.error("JPEGDEC source changed; review the pinned DSP/SRAM patch sites")
    source = source.replace(old, new)
if args.sram:
    source = """// Build-local placement profile; Pico's .data startup copy includes .time_critical.*.
#if defined(PICO_BUILD) && PICO_BUILD
#define JPEG_HOT_CODE __attribute__((section(".time_critical.jpeg_code")))
#define JPEG_HOT_DATA __attribute__((section(".time_critical.jpeg_data")))
#else
#define JPEG_HOT_CODE
#define JPEG_HOT_DATA
#endif
""" + source
for name in ("JPEGDEC.cpp", "JPEGDEC.h"):
    shutil.copyfile(args.source / name, args.output / name)
# Keep the upstream Apache-2.0 notice in the staged file.
(args.output / "jpeg.inl").write_text(source)
