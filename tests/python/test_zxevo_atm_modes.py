#!/usr/bin/env python3
"""ZX-Evo ATM video modes: 320x200 16-colour and 640x200 hardware-multicolour.

Per the BaseConf manual (v.01.01.2026) §7.1 / §7.4.4 / §7.4.5.  Each program
selects the mode (via $EFF7 + $xx77), writes known bytes to the source pages, and
HALTs.  We render and assert specific paper pixels decode per the documented
addressing:

  * ATM 320x200 16c (§7.4.5): block 0 reads b1 = page5[0].  b1=$FF packs into two
    4-bit pixels of 15 (bright white) at block-pixels 2,3; block-pixel 0 (from
    b0 = page1[0] = 0) is 0.
  * ATM 640x200 hwmc (§7.4.4): block 0 left half uses pixels=page5[0], attr=page1[0].
    page5[0]=$FF (all ink), page1[0]=$42 -> ink index Lo4($42)=10 (bright red).

Exit codes: 0 pass, 1 fail.
"""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from test_zxevo_baseconf import (ROM128, BORDER, build_sna_128k, find_inspeccy)

EXIT_HALT_DI = 2


def atm320_program() -> bytes:
    return (b"\xF3"                                   # di
            b"\x3E\xFF\x32\x00\x40"                   # ld a,$FF; ld ($4000),a  (page5[0]=b1)
            b"\xAF\x01\xF7\xEF\xED\x79"               # xor a; ld bc,$EFF7; out(c),a  (EFF7=0)
            b"\xAF\x01\x77\x03\xED\x79"               # xor a; ld bc,$0377; out(c),a  (xx77=0 ATM320)
            b"\x76")                                  # halt


def atm640_program() -> bytes:
    return (b"\xF3"                                   # di
            b"\x3E\xFF\x32\x00\x40"                   # ld a,$FF; ld ($4000),a  (page5[0]=pxL)
            b"\x3E\x01\x01\xFD\x7F\xED\x79"           # ld a,1; ld bc,$7FFD; out(c),a  (bank1->$C000)
            b"\x3E\x42\x32\x00\xC0"                   # ld a,$42; ld ($C000),a  (page1[0]=attrL)
            b"\xAF\x01\xF7\xEF\xED\x79"               # xor a; ld bc,$EFF7; out(c),a  (EFF7=0)
            b"\x3E\x02\x01\x77\x03\xED\x79"           # ld a,2; ld bc,$0377; out(c),a  (xx77=2 ATM640)
            b"\x76")                                  # halt


def read_bmp_index(path: Path, cx: int, cy: int) -> int:
    d = path.read_bytes()
    assert d[:2] == b"BM"
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    stride = ((w + 1) // 2 + 3) & ~3
    base = off + (h - 1 - cy) * stride + (cx // 2)
    return (d[base] >> 4) if (cx & 1) == 0 else (d[base] & 0x0F)


def run(exe: Path, prog: bytes, bmp: Path) -> int:
    sna = bmp.with_suffix(".sna")
    build_sna_128k(sna, prog)
    r = subprocess.run([str(exe), "--sna", str(sna), "--machine", "zxevo",
                        "--rom48128", str(ROM128), "--cycles", "300000",
                        "--bmp-out", str(bmp), "--no-dump", "--no-status",
                        "--out", str(bmp.with_suffix(".out.sna"))],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode


def main() -> int:
    exe = find_inspeccy()
    if not ROM128.is_file():
        print(f"SKIP-as-fail: {ROM128} missing"); return 1
    ok = True
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        # ATM 320x200 16c
        b = tmp / "atm320.bmp"
        run(exe, atm320_program(), b)
        p0 = read_bmp_index(b, BORDER + 0, BORDER + 0)
        p2 = read_bmp_index(b, BORDER + 2, BORDER + 0)
        if p0 != 0:
            print(f"FAIL: ATM320 pixel0 = {p0}, expected 0"); ok = False
        if p2 != 15:
            print(f"FAIL: ATM320 pixel2 = {p2}, expected 15 (bright white)"); ok = False

        # ATM 640x200 hardware-multicolour
        b = tmp / "atm640.bmp"
        run(exe, atm640_program(), b)
        q0 = read_bmp_index(b, BORDER + 0, BORDER + 0)
        if q0 != 10:
            print(f"FAIL: ATM640 pixel0 = {q0}, expected 10 (bright red ink)"); ok = False

        if ok:
            print(f"PASS: ATM 320x200 16c (px0={p0},px2={p2}) + "
                  f"ATM 640x200 hwmc (px0={q0})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
