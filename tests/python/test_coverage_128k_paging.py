#!/usr/bin/env python3
"""Coverage attribution across classic 128K bank switching.

Builds a minimal 128K .sna in-memory: code at $8000 pages banks 1, 6, 3, 0
through port $7FFD, reads one distinct address in each, then jp $0000
(RestartTrap, exit 5).  Runs it with --coverage-out and asserts each read is
attributed to the bank that was actually mapped at $C000 — the regression
this guards: WritePort7FFD used not to mirror the selected bank into
NR_56/57, so CoveragePhysIndex credited ALL $C000 traffic to the bank mapped
at load time (observed on the VooDoo demo: bank1 vertex reads and bank6
music execution all booked to bank0).

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

SLOT_SIZE      = 0x2000
RAM_PAGE_COUNT = 256                      # coverage map = 2 MiB
EXIT_RESTART_TRAP = 5

# (bank paged at $C000, address read there)
READS = [(1, 0xC123), (6, 0xC456), (3, 0xC789), (0, 0xCABC)]


def find_inspeccy():
    return locate_test_binary()


def build_program() -> bytes:
    """di; for each (bank, addr): ld bc,$7FFD / ld a,$10|bank / out (c),a /
    ld a,(addr); then jp $0000."""
    code = bytearray(b"\xF3\x01\xFD\x7F")            # di : ld bc,$7FFD
    for bank, addr in READS:
        code += bytes([0x3E, 0x10 | bank])           # ld a,$10|bank (48 ROM kept)
        code += b"\xED\x79"                          # out (c),a
        code += bytes([0x3A, addr & 0xFF, addr >> 8])  # ld a,(addr)
    code += b"\xC3\x00\x00"                          # jp $0000 -> RestartTrap
    return bytes(code)


def build_sna_128k(path: Path) -> None:
    header = bytearray(27)
    header[19] = 0x00          # IFF2: interrupts disabled
    header[25] = 0x01          # IM 1
    header[26] = 0x00          # border
    # SP is irrelevant (PC comes from the 128K extension) but keep it sane.
    header[23:25] = struct.pack("<H", 0x7FFE)

    bank = {n: bytearray(0x4000) for n in range(8)}
    prog = build_program()
    bank[2][0x0000:len(prog)] = prog                 # code at $8000

    body  = bytes(bank[5]) + bytes(bank[2]) + bytes(bank[0])   # 7FFD=$10 -> bank0 current
    ext   = struct.pack("<H", 0x8000) + bytes([0x10, 0x00])    # PC, 7FFD, TR-DOS off
    rest  = b"".join(bytes(bank[n]) for n in (1, 3, 4, 6, 7))
    path.write_bytes(bytes(header) + body + ext + rest)


def main() -> int:
    exe = find_inspeccy()
    if not ROM128.is_file():
        print(f"SKIP-as-fail: {ROM128} missing")
        return 1
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        sna = tmp / "cov128.sna"
        cov = tmp / "cov128.cov"
        build_sna_128k(sna)
        r = subprocess.run(
            [str(exe), "--sna", str(sna), "--machine", "128k",
             "--rom48128", str(ROM128), "--cycles", "200000",
             "--coverage-out", str(cov), "--no-bmp", "--no-dump", "--no-status",
             "--out", str(tmp / "cov128.out.sna")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != EXIT_RESTART_TRAP:
            print(f"FAIL: expected RestartTrap exit {EXIT_RESTART_TRAP}, "
                  f"got {r.returncode}")
            return 1
        data = cov.read_bytes()
        if len(data) != RAM_PAGE_COUNT * SLOT_SIZE:
            print(f"FAIL: coverage map size {len(data)}, "
                  f"expected {RAM_PAGE_COUNT * SLOT_SIZE}")
            return 1

        def phys(bank16k: int, cpu_addr: int) -> int:
            return bank16k * 0x4000 + (cpu_addr - 0xC000)

        ok = True
        for bank16k, addr in READS:
            idx = phys(bank16k, addr)
            if not data[idx] & 0x01:
                print(f"FAIL: read of ${addr:04X} with bank{bank16k} mapped "
                      f"not attributed to bank{bank16k} (map[{idx:#x}]="
                      f"{data[idx]:#04x})")
                ok = False
        # The old bug: everything keyed to the load-time bank (bank0).
        for bank16k, addr in READS:
            if bank16k == 0:
                continue
            idx = phys(0, addr)
            if data[idx] & 0x01:
                print(f"FAIL: read of ${addr:04X} (bank{bank16k}) leaked into "
                      f"bank0 attribution (map[{idx:#x}]={data[idx]:#04x})")
                ok = False
        # Code at $8000 executes from bank 2 (fixed slot) — sanity-check the
        # exec bit lands there.
        if not data[2 * 0x4000 + 0] & 0x02:
            print("FAIL: exec bit missing for code at $8000 (bank2)")
            ok = False
        if ok:
            print("PASS: 128K bank switching attributed correctly "
                  f"({sum(1 for b in data if b)} touched bytes)")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
