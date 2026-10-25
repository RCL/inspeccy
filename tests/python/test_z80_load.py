#!/usr/bin/env python3
""".z80 snapshot loader regression test (v1 raw, v1 RLE, v3 Timex).

sjasmplus cannot write .z80, so these fixtures are hand-built here:

  * Case 1 - v1 48K, uncompressed: a yellow paper screen ($5800 attrs = $30,
    pixels = $00) with a blue border. Program is di:halt so the loaded screen
    renders as-is. Asserts a large yellow area + blue border.
  * Case 2 - v1 48K, RLE-compressed: the SAME 49152-byte image encoded with the
    ED ED count value run scheme. Must render identically to case 1, proving the
    decompressor.
  * Case 3 - v3 Timex TC2048: hardware-mode byte 14 (auto-detects tc2048), SCLD
    mode byte ($FF) = 2 (8x1 extended colour), numbered pages. Asserts the
    machine auto-detected and the SCLD state was restored (bright-red paper from
    the $6000 8x1 attributes).

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
ROM48 = test_rom("48.rom")

# No ROM images ship with the tree; skip (exit 77) when absent.
EXIT_HALT_DELIBERATE = 6
BANK = 0x4000


def find_inspeccy():
    return locate_test_binary()


def v1_header(pc: int, sp: int, border: int, compressed: bool) -> bytearray:
    h = bytearray(30)
    h[6:8] = struct.pack("<H", pc)          # PC nonzero => v1
    h[8:10] = struct.pack("<H", sp)
    h[12] = ((border & 7) << 1) | (0x20 if compressed else 0)
    h[29] = 0x01                            # IM 1
    return h


def make_48k_ram(fill_screen) -> bytearray:
    """49152 bytes: bank5 ($4000), bank2 ($8000), bank0 ($C000). Plants di:halt
    at $8000 and lets `fill_screen(ram)` paint bank 5."""
    ram = bytearray(3 * BANK)
    ram[BANK + 0 : BANK + 2] = b"\xF3\x76"  # di:halt at $8000 (bank2 offset 0)
    fill_screen(ram)
    return ram


def yellow_screen(ram: bytearray) -> None:
    for i in range(0x1800):                  # pixels $4000-$57FF = $00 (all paper)
        ram[i] = 0x00
    for i in range(0x1800, 0x1B00):          # attrs $5800-$5AFF = $30 (yellow paper)
        ram[i] = 0x30


def rle_compress(data: bytes) -> bytes:
    """Encode with the .z80 ED ED count value scheme. Runs of >=5 (or >=2 when
    the byte is ED) collapse; ED itself is always escaped safely."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        b = data[i]
        run = 1
        while i + run < n and data[i + run] == b and run < 255:
            run += 1
        if run >= 5 or (b == 0xED and run >= 2):
            out += bytes([0xED, 0xED, run, b])
            i += run
        else:
            for _ in range(run):
                # a single ED must be followed by a non-ED literal; emit plainly
                out.append(b)
            i += run
    out += b"\x00\xED\xED\x00"               # canonical v1 end marker
    return bytes(out)


def build_z80_v1(path: Path, compressed: bool) -> None:
    ram = make_48k_ram(yellow_screen)
    body = rle_compress(bytes(ram)) if compressed else bytes(ram)
    path.write_bytes(bytes(v1_header(0x8000, 0xFF00, 1, compressed)) + body)


def build_z80_v3_timex(path: Path) -> None:
    h = bytearray(30)
    h[6:8] = struct.pack("<H", 0)            # PC=0 => extended
    h[8:10] = struct.pack("<H", 0xFF00)
    h[12] = (1 << 1)                          # blue border
    h[29] = 0x01                              # IM 1
    ext = bytearray(54)                       # v3 additional header
    ext[0:2] = struct.pack("<H", 0x8000)     # [32..33] PC
    ext[2] = 14                               # [34] hardware mode = TC2048
    ext[3] = 0                                # [35] SCLD bank ($F4)
    ext[4] = 0x02                             # [36] SCLD mode ($FF) = 8x1
    hdr = bytes(h) + struct.pack("<H", len(ext)) + bytes(ext)

    # bank contents: bank5 with 8x1 pixels+attrs; bank2 has di:halt at $8000.
    bank5 = bytearray(BANK)
    for i in range(0x0000, 0x1800):          # pixels $4000-$57FF = $FF (all ink)
        bank5[i] = 0xFF
    for i in range(0x2000, 0x3800):          # 8x1 attrs $6000-$77FF = $42 (bright red)
        bank5[i] = 0x42
    bank2 = bytearray(BANK); bank2[0:2] = b"\xF3\x76"
    bank0 = bytearray(BANK)

    def page(num: int, data: bytes) -> bytes:
        return struct.pack("<H", 0xFFFF) + bytes([num]) + data   # uncompressed

    body = page(8, bytes(bank5)) + page(4, bytes(bank2)) + page(5, bytes(bank0))
    path.write_bytes(hdr + body)


def read_bmp(path: Path):
    d = path.read_bytes()
    assert d[:2] == b"BM", "not a BMP"
    off = struct.unpack_from("<I", d, 10)[0]
    hdr_size = struct.unpack_from("<I", d, 14)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert bpp == 4, f"expected 4bpp BMP, got {bpp}"
    pal_off = 14 + hdr_size
    pal = [tuple(d[pal_off + i*4 : pal_off + i*4 + 3][::-1]) for i in range(16)]
    row = ((w + 1) // 2 + 3) & ~3
    pix = []
    for y in range(abs(h)):
        base = off + y * row
        for x in range(w):
            byte = d[base + x // 2]
            idx = (byte >> 4) if (x & 1) == 0 else (byte & 0x0F)
            pix.append(pal[idx])
    return w, abs(h), pix


def counts(bmp: Path) -> dict:
    _, _, pix = read_bmp(bmp)
    c = dict(red=0, blue=0, yellow=0)
    for (r, g, b) in pix:
        if r > 200 and g < 60 and b < 60:
            c["red"] += 1
        elif b > 150 and r < 80 and g < 80:
            c["blue"] += 1
        elif r > 190 and g > 190 and b < 80:
            c["yellow"] += 1
    return c


def run(exe: Path, args: list, tmp: Path) -> bool:
    r = subprocess.run(
        [str(exe)] + args + ["--rom48", str(ROM48), "--cycles", "200000",
         "--no-dump", "--no-status", "--out", str(tmp / "out.sna")],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != EXIT_HALT_DELIBERATE:
        print(f"FAIL: exit {r.returncode} for {args}: {r.stderr.decode()[:200]}")
        return False
    return True


def main() -> int:
    exe = find_inspeccy()
    if not ROM48.is_file():
        print(f"SKIP-as-fail: {ROM48} missing")
        return 1
    ok = True
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        # Case 1: v1 uncompressed
        z1 = tmp / "raw.z80"; b1 = tmp / "raw.bmp"
        build_z80_v1(z1, compressed=False)
        if not run(exe, ["--z80", str(z1), "--bmp-out", str(b1)], tmp):
            return 1
        c1 = counts(b1)
        if c1["yellow"] < 45_000:
            print(f"FAIL: v1 raw expected yellow paper, got {c1['yellow']} px"); ok = False
        if c1["blue"] < 10_000:
            print(f"FAIL: v1 raw expected blue border, got {c1['blue']} px"); ok = False

        # Case 2: v1 RLE-compressed -> identical render
        z2 = tmp / "rle.z80"; b2 = tmp / "rle.bmp"
        build_z80_v1(z2, compressed=True)
        if not run(exe, ["--z80", str(z2), "--bmp-out", str(b2)], tmp):
            return 1
        if b1.read_bytes() != b2.read_bytes():
            print("FAIL: v1 RLE render differs from v1 raw - decompressor bug"); ok = False

        # Case 3: v3 Timex TC2048, auto-detect + SCLD restore
        z3 = tmp / "timex.z80"; b3 = tmp / "timex.bmp"
        build_z80_v3_timex(z3)
        # no --machine: must auto-detect tc2048 from the header
        if not run(exe, ["--z80", str(z3), "--bmp-out", str(b3)], tmp):
            return 1
        c3 = counts(b3)
        if c3["red"] < 45_000:
            print(f"FAIL: v3 Timex expected bright-red 8x1 paper (SCLD mode "
                  f"restore), got {c3['red']} px"); ok = False

        if ok:
            print(f"PASS: v1 raw (yellow={c1['yellow']}, border={c1['blue']}), "
                  f"v1 RLE byte-identical, v3 Timex auto-detect + SCLD restore "
                  f"(red={c3['red']})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
