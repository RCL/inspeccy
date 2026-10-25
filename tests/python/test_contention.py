#!/usr/bin/env python3
"""Coarse contention model acceptance (--contention on).

Synthesises two 48K SNAs that run the same LDIR loop for a fixed --cycles
budget and count iterations into an uncontended RAM cell:

  variant A: LDIR src $4000 -> dst $6000   (both contended)
  variant B: LDIR src $9000 -> dst $A000   (both uncontended)

Assertions:
  1. contention OFF: A and B complete the same iteration count (the model is
     byte-identical to the uncontended emulator when off).
  2. contention ON (48K): A completes measurably fewer iterations than B
     (the penalty only hits contended pages), and B is unchanged vs OFF
     (uncontended code + data never pays).
  3. contention ON (pentagon): A == B == the OFF counts (no contended RAM).

Interrupts stay enabled-but-ignored (DI; the scheduler still anchors the
beam window off the armed frame period).

Exit 0 = pass, 1 = fail, 77 = prerequisite (ROM) missing.
"""

from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

TESTS_DIR = Path(__file__).resolve().parents[1]
INSPECCY_DIR = TESTS_DIR.parent

ROM48 = test_rom("48.rom")

# No ROM images ship with the tree; skip (exit 77) when absent.


def locate_inspeccy():
    return locate_test_binary()


COUNTER = 0xFF00          # uncontended cell holding the 16-bit iteration count


def build_sna(src: int, dst: int) -> bytes:
    """48K SNA: DI, IM 1, PC=$8000 via stacked return address at SP=$FFFE."""
    ram = bytearray(49152)

    def emit(addr: int, data: bytes) -> None:
        ram[addr - 0x4000 : addr - 0x4000 + len(data)] = data

    prog = bytes([
        0x21, src & 0xFF, src >> 8,          # ld hl, src
        0x11, dst & 0xFF, dst >> 8,          # ld de, dst
        0x01, 0x00, 0x01,                    # ld bc, $0100
        0xED, 0xB0,                          # ldir
        0x2A, COUNTER & 0xFF, COUNTER >> 8,  # ld hl, (COUNTER)
        0x23,                                # inc hl
        0x22, COUNTER & 0xFF, COUNTER >> 8,  # ld (COUNTER), hl
        0xC3, 0x00, 0x80,                    # jp $8000
    ])
    emit(0x8000, prog)
    emit(0xFFFE, bytes([0x00, 0x80]))        # stacked PC = $8000

    header = struct.pack(
        "<B HHHH HHH HH B B H H B B",
        0x3F,            # I
        0, 0, 0, 0,      # HL' DE' BC' AF'
        0, 0, 0,         # HL DE BC
        0, 0,            # IY IX
        0x00,            # IFF2 (bit 2 clear = DI)
        0x00,            # R
        0x0000,          # AF
        0xFFFE,          # SP -> stacked PC
        1,               # IM 1
        0,               # border
    )
    assert len(header) == 27, len(header)
    return header + bytes(ram)


def run(sna_path: Path, machine: str, contention: bool, binp: Path) -> int:
    """Run for a fixed cycle budget; return the iteration counter."""
    out = TESTS_DIR / "tmp" / f"cont_{machine}_{int(contention)}.out.sna"
    cmd = [
        str(binp), "--sna", str(sna_path),
        "--machine", machine,
        "--rom48", str(ROM48),
        "--cycles", "3500000",
        "--no-dump", "--no-bmp", "--no-status",
        "--out", str(out),
    ]
    # Always state it explicitly: contention may be default-on for machines with
    # contended RAM, and this test's OFF baseline must really be off.
    cmd += ["--contention", "on" if contention else "off"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL: inspeccy exit {r.returncode} for {machine} contention={contention}")
        print("      cmd: " + " ".join(cmd))
        print("      out: " + (r.stdout + r.stderr).strip()[-300:])
        print(r.stderr[-500:])
        sys.exit(1)
    data = out.read_bytes()
    off = 27 + (COUNTER - 0x4000)
    return data[off] | (data[off + 1] << 8)


def main() -> None:
    binp = locate_inspeccy()
    (TESTS_DIR / "tmp").mkdir(exist_ok=True)
    sna_a = TESTS_DIR / "tmp" / "cont_a.sna"   # contended LDIR
    sna_b = TESTS_DIR / "tmp" / "cont_b.sna"   # uncontended LDIR
    sna_a.write_bytes(build_sna(0x4000, 0x6000))
    sna_b.write_bytes(build_sna(0x9000, 0xA000))

    a_off = run(sna_a, "48k", False, binp)
    b_off = run(sna_b, "48k", False, binp)
    a_on = run(sna_a, "48k", True, binp)
    b_on = run(sna_b, "48k", True, binp)
    a_pent = run(sna_a, "pentagon", True, binp)
    b_pent = run(sna_b, "pentagon", True, binp)

    print(f"48k  off: contended={a_off} uncontended={b_off}")
    print(f"48k  on : contended={a_on} uncontended={b_on}")
    print(f"pent on : contended={a_pent} uncontended={b_pent}")

    ok = True
    if a_off != b_off:
        print("FAIL: with contention OFF the variants must run identically")
        ok = False
    if not (a_on < a_off):
        print("FAIL: contention ON must slow the contended-LDIR variant")
        ok = False
    if b_on != b_off:
        print("FAIL: contention ON must not affect uncontended code+data")
        ok = False
    if a_pent != a_off or b_pent != b_off:
        print("FAIL: pentagon must be unaffected by --contention on")
        ok = False
    # Sanity on magnitude: X=4 on both the src read and dst write while the
    # beam covers ~46% of the frame in the fetch window should cost a
    # double-digit percentage, not a rounding error and not a collapse.
    slowdown = 1.0 - (a_on / a_off) if a_off else 0.0
    print(f"contended slowdown: {slowdown * 100:.1f}%")
    if ok and not (0.02 < slowdown < 0.60):
        print("FAIL: slowdown outside plausible coarse-model range")
        ok = False

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
