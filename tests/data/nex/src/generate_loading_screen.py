#!/usr/bin/env python3
"""Reproduce loading_l2.bmp: eight diagonal colours and a white corner marker."""
import argparse
from pathlib import Path
import struct


def bitmap():
    palette = bytes.fromhex("00000000 0000e000 0080e000 00e0e000 00c00000 "
                            "c0c00000 e0000000 e000a000 a000e000 ffffff00")
    palette += bytes(1024 - len(palette))
    pixels = bytes(9 if x < 16 and y < 16 else (x + y) // 12 % 8 + 1
                   for y in reversed(range(192)) for x in range(256))
    offset = 14 + 40 + len(palette)
    header = b"BM" + struct.pack("<IHHI", offset + len(pixels), 0, 0, offset)
    header += struct.pack("<IiiHHIIiiII", 40, 256, 192, 1, 8, 0, len(pixels),
                          3780, 3780, 256, 256)
    return header + palette + pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--out", type=Path, default=Path(__file__).with_name("loading_l2.bmp"))
    args = parser.parse_args()
    expected = bitmap()
    if args.check:
        if not args.out.is_file() or args.out.read_bytes() != expected:
            print(f"Fixture mismatch: {args.out}")
            return 1
        print("Verified synthetic loading bitmap")
    else:
        args.out.write_bytes(expected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
