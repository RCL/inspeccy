#!/usr/bin/env python3
"""ZX-Uno: CoreID detection ($FC3B/$FD3B) + Timex-style 8x1 hicolour render.

Builds a 128K .sna whose code (at $8000):
  1. selects the CoreID register ($FF) on $FC3B and reads the string bytes from
     $FD3B, requiring the first two to be 'Z','X' (the inspeccy CoreID is
     "ZX-UNO"); a plain 128K floats $FF here and fails,
  2. enables Timex 8x1 hicolour via OUT 255,2 (the ZX-Uno honours it),
  3. fills pixel lines 0/1 all-ink and writes 8x1 attributes (row0 bright-red,
     row1 bright-green) at pixel_addr+$2000 in bank 5,
  4. jp $0000 (RestartTrap, exit 5).

Asserts the render shows red on line 0 and green on line 1 (8x1 active), and a
--machine 128k control does NOT detect (exit 2).  Exit codes: 0 pass, 1 fail.
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

BORDER  = 48
CANVAS_W, CANVAS_H = 352, 288
STRIDE  = CANVAS_W // 2
IDX_RED_BRIGHT, IDX_GREEN_BRIGHT = 10, 12
EXIT_RESTART_TRAP, EXIT_HALT_DI = 5, 2


def find_inspeccy():
    return locate_test_binary()


def fill_loop(dst: int, val: int) -> bytes:
    return (bytes([0x21, dst & 0xFF, dst >> 8, 0x06, 0x20]) +
            bytes([0x36, val, 0x23, 0x10, 0xFB]))


def build_program() -> bytes:
    p = bytearray()
    p += b"\xF3"                                   # di
    p += b"\x3E\xFF\x01\x3B\xFC\xED\x79"           # ld a,$FF; ld bc,$FC3B; out (c),a
    p += b"\x01\x3B\xFD"                           # ld bc,$FD3B
    p += b"\xED\x78\xFE\x5A"                       # in a,(c); cp 'Z'
    jr1 = len(p); p += b"\x20\x00"                 # jr nz,fail (patch)
    p += b"\xED\x78\xFE\x58"                       # in a,(c); cp 'X'
    jr2 = len(p); p += b"\x20\x00"                 # jr nz,fail (patch)
    p += b"\x3E\x02\xD3\xFF"                       # ld a,2; out ($FF),a  (Timex mode 2)
    p += fill_loop(0x4000, 0xFF)                   # row0 pixels
    p += fill_loop(0x4100, 0xFF)                   # row1 pixels
    p += fill_loop(0x6000, 0x42)                   # row0 8x1 attr bright-red
    p += fill_loop(0x6100, 0x44)                   # row1 8x1 attr bright-green
    p += b"\xC3\x00\x00"                           # jp $0000 -> pass
    fail = len(p)
    p += b"\x76"                                   # fail: halt -> exit 2
    p[jr1 + 1] = (fail - (jr1 + 2)) & 0xFF
    p[jr2 + 1] = (fail - (jr2 + 2)) & 0xFF
    return bytes(p)


def build_sna_128k(path: Path, prog: bytes) -> None:
    header = bytearray(27)
    header[19] = 0x00; header[25] = 0x01; header[26] = 0x00
    header[23:25] = struct.pack("<H", 0x7FFE)
    bank = {n: bytearray(0x4000) for n in range(8)}
    bank[2][0:len(prog)] = prog
    body = bytes(bank[5]) + bytes(bank[2]) + bytes(bank[0])
    ext  = struct.pack("<H", 0x8000) + bytes([0x10, 0x00])
    rest = b"".join(bytes(bank[n]) for n in (1, 3, 4, 6, 7))
    path.write_bytes(bytes(header) + body + ext + rest)


def bmp_index_at(path: Path, cx: int, cy: int) -> int:
    d = path.read_bytes()
    off = struct.unpack_from("<I", d, 10)[0]
    base = off + (CANVAS_H - 1 - cy) * STRIDE + (cx // 2)
    return (d[base] >> 4) if (cx & 1) == 0 else (d[base] & 0x0F)


def run(exe: Path, machine: str, bmp: Path) -> int:
    sna = bmp.with_suffix(".sna")
    build_sna_128k(sna, build_program())
    r = subprocess.run(
        [str(exe), "--sna", str(sna), "--machine", machine,
         "--rom48128", str(ROM128), "--cycles", "300000",
         "--bmp-out", str(bmp), "--no-dump", "--no-status", "--out",
         str(bmp.with_suffix(".out.sna"))],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode


def main() -> int:
    exe = find_inspeccy()
    if not ROM128.is_file():
        print(f"SKIP-as-fail: {ROM128} missing"); return 1
    ok = True
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        bmp = tmp / "uno.bmp"
        rc = run(exe, "zxuno", bmp)
        if rc != EXIT_RESTART_TRAP:
            print(f"FAIL: zxuno expected {EXIT_RESTART_TRAP}, got {rc} "
                  f"(CoreID detect failed?)"); return 1
        line0 = bmp_index_at(bmp, BORDER + 4, BORDER + 0)
        line1 = bmp_index_at(bmp, BORDER + 4, BORDER + 1)
        if line0 != IDX_RED_BRIGHT:
            print(f"FAIL: line0 = idx {line0}, expected red {IDX_RED_BRIGHT}"); ok = False
        if line1 != IDX_GREEN_BRIGHT:
            print(f"FAIL: line1 = idx {line1}, expected green {IDX_GREEN_BRIGHT} "
                  f"(8x1 not active)"); ok = False
        rc2 = run(exe, "128k", tmp / "p128.bmp")
        if rc2 != EXIT_HALT_DI:
            print(f"FAIL: 128k control expected {EXIT_HALT_DI}, got {rc2}"); ok = False
        if ok:
            print(f"PASS: ZX-Uno CoreID detect + Timex 8x1 hicolour "
                  f"(line0=red{line0}, line1=green{line1})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
