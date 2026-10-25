#!/usr/bin/env python3
"""Build the small instruction-only SNA fixtures, or verify them with --check.

Uses Python's standard library on Windows, Linux and macOS. Each image contains
zero-filled RAM, a short program at $8000 and a 128K SNA header/trailer. No ROM
bytes, screen assets or external programs are used.
"""
import argparse
from pathlib import Path


PROGRAMS = {
    "screen_pixel_write": "3e ff 32 00 40 f3 76",
    "screen_attr_paint": "3e 07 32 00 58 f3 76",
    "halt_ei": "fb 76 18 fe",
    "border_red": "3e 02 d3 fe f3 76",
    "uniform_white": "3e 38 32 00 58 21 00 58 11 01 58 01 ff 02 ed b0 f3 76",
    "halt_di_nop_halt": "f3 00 76",
    "halt_bug": "f3 " + "00 " * 100 + "76",
    "nop_loop": "00 18 fd",
    "restart_into_rom": "f3 31 00 90 c9",
    "otir_to_7ffd": "21 0c 80 01 fd 7f ed a3 f3 76 18 fe 11",
    "psg_mask_r1": "01 fd ff 3e 01 ed 79 01 fd bf 3e ff ed 79 f3 76",
    "psg_silent": "f3 76",
}


def snapshot(code):
    data = bytearray(131103)
    data[25:27] = bytes((1, 7))  # IM 1, white border
    start = 27 + 16384          # bank 2 at $8000
    data[start:start + len(code)] = code
    trailer = 27 + 3 * 16384
    data[trailer:trailer + 4] = bytes((0, 0x80, 0, 0))  # PC, 7FFD, TR-DOS
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    failed = []
    for name, code in PROGRAMS.items():
        path = args.out / (name + ".sna")
        expected = snapshot(bytes.fromhex(code))
        if args.check:
            if not path.is_file() or path.read_bytes() != expected:
                failed.append(path.name)
        else:
            args.out.mkdir(parents=True, exist_ok=True)
            path.write_bytes(expected)
    if failed:
        print("Fixture mismatch: " + ", ".join(failed))
        return 1
    print(f"{'Verified' if args.check else 'Wrote'} {len(PROGRAMS)} synthetic SNA fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
