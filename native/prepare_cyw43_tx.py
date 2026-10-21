#!/usr/bin/env python3
"""Stage a bounded data-TX wait in the pinned CYW43 driver, leaving the SDK intact."""
import argparse
import hashlib
from pathlib import Path

PINNED_SHA256 = "7f4c93755b2ab911d18606652497b2f3cd08085ce63b3b1836171d375f806b10"
ORIGINAL = "if (cur_us - start_us > 1000000) {"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--data-wait-us", type=int, default=8000)
args = parser.parse_args()
if not 1000 <= args.data_wait_us <= 25000:
    parser.error("data wait must be between 1000 and 25000 microseconds")
if args.source.resolve() == args.output.resolve():
    parser.error("the output must be a build-local copy, never the SDK source")
original = args.source.read_bytes()
if hashlib.sha256(original).hexdigest() != PINNED_SHA256:
    parser.error("CYW43 driver revision changed; review the data-TX wait before building")
source = original.decode("utf8")
if source.count(ORIGINAL) != 1:
    parser.error("CYW43 flow-control wait changed; review its timeout and cleanup")
# During an AP restart the radio can stop granting SDPCM credits. The stock
# one-second synchronous wait freezes core 0, including buttons and the LCD.
# Data packets can fail promptly: UDP retries on its normal cadence and lwIP
# retains/retransmits TCP data. Keep startup/configuration IOCTL timing intact.
replacement = (
    "// streamer: bound only Ethernet data; retain the SDK control/IOCTL timeout.\n"
    f"            if (cur_us - start_us > (kind == DATA_HEADER ? {args.data_wait_us}u : 1000000u)) {{"
)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(source.replace(ORIGINAL, replacement))
