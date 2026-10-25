#!/usr/bin/env python3
"""ZX Evolution BaseConf: detection port + hardware-multicolour render.

Builds a minimal 128K .sna whose code (at $8000):
  1. reads $FFBF and requires bits 7:6 == 0 (the BaseConf config-read port; on a
     plain 128K it floats $FF and this fails),
  2. fills pixel rows 0 and 1 with all-ink ($FF) in the display bank (bank 5),
  3. writes 8x1 attributes at pixel_addr+$2000: row 0 = bright-red ink ($42),
     row 1 = bright-green ink ($44),
  4. enables Pentagon hardware-multicolour via $EFF7 bit 5,
  5. jp $0000 (RestartTrap, exit 5).

Then renders with --machine zxevo --bmp-out and asserts the paper pixel on
display line 0 is bright red and on line 1 is bright green.  In an 8x8 mode both
lines would share line 0's attribute (both red) — the green on line 1 proves the
8x1 hardware-multicolour path is active.  A --machine 128k control must NOT
detect (exit 2) — proving the port is correctly gated.

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

BORDER  = 48
CANVAS_W, CANVAS_H = 352, 288
STRIDE  = CANVAS_W // 2
IDX_RED_BRIGHT   = 10
IDX_GREEN_BRIGHT = 12
EXIT_RESTART_TRAP = 5
EXIT_HALT_DI      = 2


def find_inspeccy():
    return locate_test_binary()


def fill_loop(dst: int, val: int) -> bytes:
    """ld hl,dst / ld b,32 / (ld (hl),val / inc hl / djnz)."""
    return (bytes([0x21, dst & 0xFF, dst >> 8, 0x06, 0x20]) +
            bytes([0x36, val, 0x23, 0x10, 0xFB]))


def build_program() -> bytes:
    # Detect via $FFBF; on failure fall through to halt.
    head = (b"\xF3"                       # di
            b"\x01\xBF\xFF"               # ld bc,$FFBF
            b"\xED\x78"                   # in a,(c)
            b"\xE6\xC0")                  # and $C0  (bits 7:6)
    body = (fill_loop(0x4000, 0xFF) +     # row 0 pixels ($4000-$401F)
            fill_loop(0x4100, 0xFF) +     # row 1 pixels ($4100-$411F)
            fill_loop(0x6000, 0x42) +     # row 0 8x1 attr: bright red ink
            fill_loop(0x6100, 0x44) +     # row 1 8x1 attr: bright green ink
            b"\x3E\x20"                   # ld a,$20  (EFF7 bit5 = HWMC)
            b"\x01\xF7\xEF"               # ld bc,$EFF7
            b"\xED\x79"                   # out (c),a
            b"\xC3\x00\x00")              # jp $0000 -> RestartTrap
    fail = b"\x76"                        # halt -> exit 2
    # jr nz,fail : displacement = len(body) (skip straight to halt)
    assert len(body) < 128
    return head + bytes([0x20, len(body)]) + body + fail


def build_paging_program() -> bytes:
    """Prove the ATM/PentEvo page-window selects distinct physical RAM pages.
    Window 2 ($8000) is pointed at 16K RAM page 20, then 21, then 20 again via
    the $B7F7 PentEvo manager (page = value XOR $FF); a byte written under the
    first mapping must reappear only when that same page is re-selected.
    """
    def win2(page16k: int) -> bytes:                 # $B7F7 <- page ^ 0xFF
        return bytes([0x3E, page16k ^ 0xFF, 0x01, 0xF7, 0xB7, 0xED, 0x79])
    return (b"\xF3"                                   # di
            + win2(20) + b"\x3E\xAA\x32\x00\x80"      # page20: ld a,$AA; ld ($8000),a
            + win2(21) + b"\x3E\xBB\x32\x00\x80"      # page21: ld a,$BB; ld ($8000),a
            + win2(20) + b"\x3A\x00\x80"              # page20 again: ld a,($8000)
            + b"\xFE\xAA"                             # cp $AA
            + b"\x20\x01"                             # jr nz,fail
            + b"\xC3\x00\x00"                         # jp $0000 -> pass (exit 5)
            + b"\x76")                                # fail: halt (exit 2)


def build_sna_128k(path: Path, prog: bytes) -> None:
    header = bytearray(27)
    header[19] = 0x00; header[25] = 0x01; header[26] = 0x00
    header[23:25] = struct.pack("<H", 0x7FFE)
    bank = {n: bytearray(0x4000) for n in range(8)}
    bank[2][0:len(prog)] = prog                   # code at $8000 (bank 2 fixed)
    body = bytes(bank[5]) + bytes(bank[2]) + bytes(bank[0])
    ext  = struct.pack("<H", 0x8000) + bytes([0x10, 0x00])   # PC=$8000, 7FFD=$10, TR-DOS off
    rest = b"".join(bytes(bank[n]) for n in (1, 3, 4, 6, 7))
    path.write_bytes(bytes(header) + body + ext + rest)


def bmp_index_at(path: Path, canvas_x: int, canvas_y: int) -> int:
    d = path.read_bytes()
    assert d[:2] == b"BM"
    off = struct.unpack_from("<I", d, 10)[0]
    row_from_bottom = CANVAS_H - 1 - canvas_y
    base = off + row_from_bottom * STRIDE + (canvas_x // 2)
    byte = d[base]
    return (byte >> 4) if (canvas_x & 1) == 0 else (byte & 0x0F)


def run(exe: Path, machine: str, prog: bytes, bmp: Path, out: Path) -> int:
    sna = bmp.with_suffix(".sna")
    build_sna_128k(sna, prog)
    r = subprocess.run(
        [str(exe), "--sna", str(sna), "--machine", machine,
         "--rom48128", str(ROM128), "--cycles", "300000",
         "--bmp-out", str(bmp), "--no-dump", "--no-status", "--out", str(out)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode


def main() -> int:
    exe = find_inspeccy()
    if not ROM128.is_file():
        print(f"SKIP-as-fail: {ROM128} missing"); return 1
    ok = True
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # ZX-Evo: detects + renders HWMC.
        bmp = tmp / "evo.bmp"
        rc = run(exe, "zxevo", build_program(), bmp, tmp / "evo.out.sna")
        if rc != EXIT_RESTART_TRAP:
            print(f"FAIL: zxevo expected RestartTrap {EXIT_RESTART_TRAP}, got {rc} "
                  f"($FFBF detect failed?)"); return 1
        line0 = bmp_index_at(bmp, BORDER + 4, BORDER + 0)
        line1 = bmp_index_at(bmp, BORDER + 4, BORDER + 1)
        if line0 != IDX_RED_BRIGHT:
            print(f"FAIL: HWMC line 0 = idx {line0}, expected bright red "
                  f"{IDX_RED_BRIGHT}"); ok = False
        if line1 != IDX_GREEN_BRIGHT:
            print(f"FAIL: HWMC line 1 = idx {line1}, expected bright green "
                  f"{IDX_GREEN_BRIGHT} (8x1 not active -> would inherit line 0's red)")
            ok = False
        # 128K control: must NOT detect (port floats $FF -> halt, exit 2).
        rc2 = run(exe, "128k", build_program(), tmp / "p128.bmp", tmp / "p128.out.sna")
        if rc2 != EXIT_HALT_DI:
            print(f"FAIL: 128k control expected halt {EXIT_HALT_DI} (no $FFBF), "
                  f"got {rc2}"); ok = False
        # Page-window: A/B/A remap must select distinct physical pages.
        rc3 = run(exe, "zxevo", build_paging_program(), tmp / "pg.bmp", tmp / "pg.out.sna")
        if rc3 != EXIT_RESTART_TRAP:
            print(f"FAIL: page-window remap expected {EXIT_RESTART_TRAP}, got {rc3} "
                  f"($8000 didn't follow the window override)"); ok = False
        if ok:
            print(f"PASS: BaseConf detect + HWMC 8x1 render "
                  f"(line0=red{line0}, line1=green{line1}) + page-window remap")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
