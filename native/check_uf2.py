"""Reject native UF2 files that could touch Badgeware user storage."""
from pathlib import Path
import struct
import sys


def check(path):
    data = Path(path).read_bytes()
    if not data or len(data) % 512:
        raise ValueError("UF2 must contain complete 512-byte blocks")
    guard_found = False
    block_numbers = set()
    ordinary_count = None
    for offset in range(0, len(data), 512):
        first, second, flags, address, length, number, count, family = struct.unpack_from("<8I", data, offset)
        if (first, second, struct.unpack_from("<I", data, offset + 508)[0]) != (
                0x0A324655, 0x9E5D5157, 0x0AB16F30):
            raise ValueError("invalid UF2 magic")
        if not 0 < length <= 476:
            raise ValueError("invalid UF2 payload length")
        if address == 0x101FFF00 and length == 256:
            # Pico SDK's RP2350-E10 compatibility block. A2 boot ROMs can
            # write this despite its ignore flag; its ADDRESS must be safe.
            guard_found = True
        else:
            if not 0x10000000 <= address < address + length <= 0x101FF000:
                raise ValueError("ordinary UF2 data overlaps the guard sector or Badgeware storage")
            if ordinary_count is None:
                ordinary_count = count
            if count != ordinary_count or number >= count or number in block_numbers:
                raise ValueError("inconsistent ordinary UF2 block numbering")
            block_numbers.add(number)
    if not guard_found:
        raise ValueError("missing safe RP2350-E10 guard at 0x101fff00")
    if not ordinary_count or len(block_numbers) != ordinary_count:
        raise ValueError("missing ordinary UF2 blocks")
    return len(data) // 512


if __name__ == "__main__":
    count = check(sys.argv[1])
    print("UF2 range check: %d blocks, safe E10 guard, all writes below 2 MiB" % count)
