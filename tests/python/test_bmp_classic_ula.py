#!/usr/bin/env python3
"""Classic-machine ULA compositing regression test.

Builds a minimal 128K .sna whose code sets border blue, fills the attributes
with $38 (white paper / black ink) and writes one $FF pixel byte, then runs
it with --bmp-out and asserts the composite contains the three expected
colour families (white paper, blue border, black ink pixels).

Guards the regression fixed 2026-07-18: palettes and NR_14 used to zero-init,
so every ULA pixel matched the transparency test and classic .sna machines
produced uniform border-colour-only BMPs ("flat frames") from --bmp-out and
--video-out.

Exit codes: 0 = pass, 1 = fail.
"""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

HERE         = Path(__file__).resolve().parents[1]
INSPECCY_DIR = HERE.parent
BIN_CANDIDATES = binary_candidates()
ROM128 = test_rom("128.rom")

# No ROM images ship with the tree; skip (exit 77) when absent.
EXIT_HALT_DELIBERATE = 6


def find_inspeccy():
    return locate_test_binary()


def build_program() -> bytes:
    code = bytearray()
    code += b"\xF3"                          # di
    code += b"\x3E\x01\xD3\xFE"              # ld a,1 : out ($FE),a  -> blue border
    code += b"\x21\x00\x58"                  # ld hl,$5800
    code += b"\x11\x01\x58"                  # ld de,$5801
    code += b"\x01\xFF\x02"                  # ld bc,$02FF
    code += b"\x36\x38"                      # ld (hl),$38  (white paper, black ink)
    code += b"\xED\xB0"                      # ldir
    code += b"\x3E\xFF\x32\x00\x40"          # ld a,$FF : ld ($4000),a  (8 ink pixels)
    code += b"\xF3\x76"                      # di : halt  -> HaltedDeliberateStop
    return bytes(code)


def build_sna_128k(path: Path) -> None:
    header = bytearray(27)
    header[25] = 0x01                        # IM 1
    header[23:25] = struct.pack("<H", 0x7FFE)
    bank = {n: bytearray(0x4000) for n in range(8)}
    prog = build_program()
    bank[2][0:len(prog)] = prog              # code at $8000
    body = bytes(bank[5]) + bytes(bank[2]) + bytes(bank[0])
    ext = struct.pack("<H", 0x8000) + bytes([0x10, 0x00])
    rest = b"".join(bytes(bank[n]) for n in (1, 3, 4, 6, 7))
    path.write_bytes(bytes(header) + body + ext + rest)


def read_bmp_pixels(path: Path):
    """Yields (r,g,b) for 24bpp and 4bpp-palettized BMPs (the composite
    writers emit both: --video-out 24bpp, --bmp-out 16-colour)."""
    d = path.read_bytes()
    assert d[:2] == b"BM", "not a BMP"
    off = struct.unpack_from("<I", d, 10)[0]
    hdr_size = struct.unpack_from("<I", d, 14)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp == 24:
        row = (w * 3 + 3) & ~3
        for y in range(abs(h)):
            base = off + y * row
            for x in range(w):
                b, g, r = d[base + x*3 : base + x*3 + 3]
                yield (r, g, b)
    elif bpp == 4:
        pal_off = 14 + hdr_size
        pal = [tuple(d[pal_off + i*4 : pal_off + i*4 + 3][::-1])  # BGRx -> RGB
               for i in range(16)]
        row = ((w + 1) // 2 + 3) & ~3
        for y in range(abs(h)):
            base = off + y * row
            for x in range(w):
                byte = d[base + x // 2]
                idx = (byte >> 4) if (x & 1) == 0 else (byte & 0x0F)
                yield pal[idx]
    else:
        raise AssertionError(f"unhandled bpp {bpp}")


def main() -> int:
    exe = find_inspeccy()
    if not ROM128.is_file():
        print(f"SKIP-as-fail: {ROM128} missing")
        return 1
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        sna = tmp / "ula.sna"
        bmp = tmp / "ula.bmp"
        build_sna_128k(sna)
        r = subprocess.run(
            [str(exe), "--sna", str(sna), "--machine", "128k",
             "--rom48128", str(ROM128), "--cycles", "500000",
             "--bmp-out", str(bmp), "--no-dump", "--no-status",
             "--out", str(tmp / "ula.out.sna")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != EXIT_HALT_DELIBERATE:
            print(f"FAIL: expected exit {EXIT_HALT_DELIBERATE}, got {r.returncode}")
            return 1
        white = blue = black = 0
        colours = set()
        for (r_, g_, b_) in read_bmp_pixels(bmp):
            colours.add((r_, g_, b_))
            if r_ > 190 and g_ > 190 and b_ > 190:
                white += 1
            elif b_ > 150 and r_ < 80 and g_ < 80:
                blue += 1
            elif r_ < 30 and g_ < 30 and b_ < 30:
                black += 1
        ok = True
        if len(colours) < 3:
            print(f"FAIL: composite is (near-)uniform: {len(colours)} distinct "
                  f"colours - the old flat-frame regression")
            ok = False
        if white < 45_000:      # paper area is 256x192 = 49152 px
            print(f"FAIL: expected a large white paper area, got {white} px")
            ok = False
        if blue < 10_000:
            print(f"FAIL: expected a blue border, got {blue} px")
            ok = False
        if black < 4:
            print(f"FAIL: expected the 8 ink pixels, got {black} px")
            ok = False
        if ok:
            print(f"PASS: classic ULA composite has paper ({white}), border "
                  f"({blue}) and ink ({black}) pixels, {len(colours)} colours")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
