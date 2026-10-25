#!/usr/bin/env python3
"""--until-pc <addr>: a real PC breakpoint. Stops BEFORE executing the target
instruction, with state as of that instant; result=ReachedPc, exit code 8.
'PP:NNNN' also requires the 8K physical page PP mapped at the PC's slot (a
banked / 24-bit breakpoint), so the same Z80 address in a different bank does
not false-fire.

Synthesises a 48K program:  4x nop ; ld a,5 ; loop: inc a ; jr loop
and checks:
  - --until-pc at each instruction boundary stops there (exit 8, PC == target)
  - no --until-pc runs to the cycle budget (exit 0)
  - a page that IS mapped fires; a page that is NOT does not (runs to budget)

Exit 0 = pass, 1 = fail, 77 = prerequisite (ROM) missing.
"""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

TESTS_DIR = Path(__file__).resolve().parents[1]
INSPECCY_DIR = TESTS_DIR.parent
ROM48 = test_rom("48.rom")

ORG = 0x8000
EXIT_BUDGET = 0
EXIT_REACHED_PC = 8


def make_sna(path: Path) -> None:
    ram = bytearray(49152)
    #        8000 8001 8002 8003  8004:ld a,5  8006:inc a  8007: jr -1
    code = bytes([0x00, 0x00, 0x00, 0x00, 0x3E, 0x05, 0x3C, 0x18, 0xFD])
    ram[ORG - 0x4000:ORG - 0x4000 + len(code)] = code
    sp = 0xFF00
    ram[sp - 2 - 0x4000] = ORG & 0xFF
    ram[sp - 1 - 0x4000] = ORG >> 8
    hdr = struct.pack("<BHHHHHHHHHBBHHBB", 0x3F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, sp - 2, 1, 7)
    path.write_bytes(hdr + bytes(ram))


def run(inspeccy: str, sna: Path, work: Path, machine: str, extra: list[str], dump: bool = False):
    dm = work / "d.json"
    cmd = [inspeccy, "--sna", str(sna), "--machine", machine, "--rom48", str(ROM48),
           "--cycles", "100000", "--no-interrupts", "--no-bmp", "--no-status"] + extra
    cmd += (["--dump-out", str(dm)] if dump else ["--no-dump"])
    p = subprocess.run(cmd, capture_output=True, text=True)
    pc = None
    if dump and dm.is_file():
        cpu = json.loads(dm.read_text())["cpu"]
        pc = int(str(cpu["PC"]).lstrip("$"), 16)
    return p.returncode, pc


def main() -> int:
    inspeccy = str(locate_test_binary())
    args = sys.argv[1:]
    if "--inspeccy" in args:
        inspeccy = args[args.index("--inspeccy") + 1]
    if not inspeccy:
        for c in (INSPECCY_DIR / "build" / "inspeccy" / "Release" / "inspeccy.exe",
                  INSPECCY_DIR / "build" / "inspeccy" / "inspeccy"):
            if c.is_file():
                inspeccy = str(c)
                break
    if not inspeccy:
        print("inspeccy binary not found")
        return 1
    if not ROM48.is_file():
        print(f"SKIP: missing {ROM48}")
        return 77

    fails: list[str] = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    with tempfile.TemporaryDirectory(prefix="until_pc_") as td:
        work = Path(td)
        sna = work / "bp.sna"
        make_sna(sna)

        # break at each instruction boundary
        for tgt in (0x8000, 0x8004, 0x8006):
            rc, pc = run(inspeccy, sna, work, "48k", ["--until-pc", hex(tgt)], dump=True)
            check(rc == EXIT_REACHED_PC, f"--until-pc {tgt:#06x}: expected exit {EXIT_REACHED_PC}, got {rc}")
            check(pc == tgt, f"--until-pc {tgt:#06x}: stopped at PC {pc:#06x} not {tgt:#06x}"
                  if pc is not None else f"--until-pc {tgt:#06x}: no PC in dump")

        # no breakpoint -> runs to budget
        rc, _ = run(inspeccy, sna, work, "48k", [])
        check(rc == EXIT_BUDGET, f"no --until-pc: expected budget exit {EXIT_BUDGET}, got {rc}")

        # banked form: $8000's slot maps to 8K page 4 on the 48K layout.
        rc, _ = run(inspeccy, sna, work, "48k", ["--until-pc", "4:0x8006"])
        check(rc == EXIT_REACHED_PC, f"--until-pc 4:8006 (right page): expected {EXIT_REACHED_PC}, got {rc}")
        rc, _ = run(inspeccy, sna, work, "48k", ["--until-pc", "5:0x8006"])
        check(rc == EXIT_BUDGET, f"--until-pc 5:8006 (wrong page): expected budget {EXIT_BUDGET}, got {rc}")

    for f in fails:
        print("FAIL: " + f)
    print("until_pc: " + ("ok" if not fails else f"{len(fails)} failure(s)"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
