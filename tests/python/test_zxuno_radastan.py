#!/usr/bin/env python3
"""ZX-Uno Radastan (128x96 4bpp) render, per the ZX-Uno core (ula_radas.v).

The program: writes framebuffer byte bank5[0]=$50 (left pixel nibble 5, right 0);
sets ULAplus CRAM[5] = $1C (GRB 3-3-2 = red: R=7,G=0,B=0) via $BF3B/$FF3B; enables
Radastan via RADASCTRL ($40) = 3 on $FC3B/$FD3B; then HALTs.  The 24-bit render
must show paper pixel (0,0) bright red and (1,0) black (CRAM[0] default).

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
from test_zxevo_baseconf import ROM128, build_sna_128k, find_inspeccy

BORDER = 32


def radastan_program() -> bytes:
    return (b"\xF3"                                   # di
            b"\x3E\x50\x32\x00\x40"                   # ld a,$50; ld ($4000),a  (framebuffer)
            b"\x01\x3B\xBF\x3E\x05\xED\x79"           # ld bc,$BF3B; ld a,5; out(c),a  (ULAplus reg 5)
            b"\x01\x3B\xFF\x3E\x1C\xED\x79"           # ld bc,$FF3B; ld a,$1C; out(c),a  (CRAM[5]=red)
            b"\x01\x3B\xFC\x3E\x40\xED\x79"           # ld bc,$FC3B; ld a,$40; out(c),a  (sel RADASCTRL)
            b"\x01\x3B\xFD\x3E\x03\xED\x79"           # ld bc,$FD3B; ld a,3; out(c),a  (Radastan on)
            b"\x76")                                  # halt


def read_bmp24(path: Path, cx: int, cy: int):
    d = path.read_bytes()
    assert d[:2] == b"BM"
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert bpp == 24, f"expected 24bpp, got {bpp}"
    stride = (w * 3 + 3) & ~3
    base = off + (h - 1 - cy) * stride + cx * 3
    b, g, r = d[base], d[base + 1], d[base + 2]
    return (r, g, b)


def main() -> int:
    exe = find_inspeccy()
    if not ROM128.is_file():
        print(f"SKIP-as-fail: {ROM128} missing"); return 1
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        sna = tmp / "radas.sna"
        bmp = tmp / "radas.bmp"
        build_sna_128k(sna, radastan_program())
        subprocess.run([str(exe), "--sna", str(sna), "--machine", "zxuno",
                        "--rom48128", str(ROM128), "--cycles", "300000",
                        "--bmp-out", str(bmp), "--no-dump", "--no-status",
                        "--out", str(tmp / "radas.out.sna")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not bmp.is_file():
            print("FAIL: no BMP rendered"); return 1
        px0 = read_bmp24(bmp, BORDER + 0, BORDER + 0)
        px1 = read_bmp24(bmp, BORDER + 1, BORDER + 0)
        ok = True
        if px0 != (255, 0, 0):
            print(f"FAIL: Radastan pixel0 RGB = {px0}, expected (255,0,0) red"); ok = False
        if px1 != (0, 0, 0):
            print(f"FAIL: Radastan pixel1 RGB = {px1}, expected (0,0,0) black"); ok = False
        if ok:
            print(f"PASS: ZX-Uno Radastan 128x96 4bpp render (px0={px0}, px1={px1})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
