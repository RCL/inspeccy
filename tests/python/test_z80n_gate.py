#!/usr/bin/env python3
"""Z80N opcodes exist only on the Next's CPU core.

Synthesises a 48K SNA whose program is

    nextreg $07,3        ; turbo 28 MHz  (ED 91 07 03)
    nextreg $69,2        ; SCLD mode 2   (ED 91 69 02)
    di
    halt

and runs it on several machines:

  1. next50: both writes land - the run stops on the DI;HALT (exit 6) at
     28 MHz with NR $69 bits 5:0 = 2.
  2. 48k / tc2048 / pentagon / zxevo / zxuno: ED 91 is an undocumented slot on
     a real Z80, so the run trips InvalidOpcode (exit 1) at PC $8000 and the
     clock and SCLD mode are exactly what a bare DI;HALT leaves on that machine
     (the Evo boots at 7 MHz by design). On the Timex in particular, the NR $69
     alias of port $FF must NOT switch the display mode.
  3. tc2048 with --lenient-undoc-ed: the two ED 91 are 8 T no-ops; the run
     reaches the DI;HALT (exit 6), clock and SCLD mode unchanged, and the
     operand bytes execute as instructions on the way ($07 = rlca, $03 =
     inc bc, $69 = ld l,c, $02 = ld (bc),a) - 37 T more than the bare
     DI;HALT, which is exactly what real hardware would do.

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

CODE = bytes([0xED, 0x91, 0x07, 0x03,   # nextreg $07,3
              0xED, 0x91, 0x69, 0x02,   # nextreg $69,2
              0xF3, 0x76])              # di ; halt
CONTROL = bytes([0xF3, 0x76])           # di ; halt - the per-machine baseline
ORG = 0x8000


def build_sna(path: Path, code: bytes = CODE) -> None:
    ram = bytearray(49152)
    ram[ORG - 0x4000:ORG - 0x4000 + len(code)] = code
    sp = 0xFF00
    ram[sp - 2 - 0x4000] = ORG & 0xFF
    ram[sp - 1 - 0x4000] = ORG >> 8
    hdr = struct.pack("<BHHHHHHHHHBBHHBB", 0x3F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, sp - 2, 1, 7)
    path.write_bytes(hdr + bytes(ram))


def nr69(dump: dict):
    """NextRegs[$69] from the dump (Next only), or None."""
    import base64
    b64 = dump.get("next", {}).get("nextregs_b64") if isinstance(dump.get("next"), dict) else None
    if not b64:
        return None
    return base64.b64decode(b64)[0x69]


def run(inspeccy: str, sna: Path, machine: str, work: Path, extra: list[str]) -> tuple[int, dict, dict]:
    tag = f"{machine}_{abs(hash((sna.name, tuple(extra))))}"
    st = work / f"{tag}.status.json"
    dm = work / f"{tag}.dump.json"
    cmd = [inspeccy, "--sna", str(sna), "--machine", machine, "--rom48", str(ROM48),
           "--cycles", "2000", "--no-interrupts", "--no-bmp", "--out", str(work / f"{machine}.out.sna"),
           "--status-out", str(st), "--dump-out", str(dm)] + extra
    p = subprocess.run(cmd, capture_output=True, text=True)
    status = json.loads(st.read_text()) if st.is_file() else {}
    dump = json.loads(dm.read_text()) if dm.is_file() else {}
    return p.returncode, status, dump


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
        print("SKIP: no 48.rom")
        return 77

    fails: list[str] = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    with tempfile.TemporaryDirectory(prefix="z80n_gate_") as td:
        work = Path(td)
        sna = work / "in.sna"
        build_sna(sna)
        ctl = work / "control.sna"
        build_sna(ctl, CONTROL)

        rc, st, dm = run(inspeccy, sna, "next50", work, [])
        check(rc == 6, f"next50: expected exit 6 (DI;HALT), got {rc} {st.get('exit')}")
        check(st.get("mhz") == 28.0, f"next50: expected 28 MHz after nextreg $07, got {st.get('mhz')}")
        check(nr69(dm) is not None and (nr69(dm) & 0x3F) == 2,
              f"next50: expected NR $69 bits 5:0 = 2 after nextreg $69, got {nr69(dm)}")

        for machine in ("48k", "tc2048", "pentagon", "zxevo", "zxuno"):
            # baseline: what this machine reports for a bare DI;HALT (the Evo
            # boots at 7 MHz by design, so the clock is compared, not assumed)
            _, st0, dm0 = run(inspeccy, ctl, machine, work, [])
            rc, st, dm = run(inspeccy, sna, machine, work, [])
            check(rc == 1, f"{machine}: expected InvalidOpcode (exit 1) on ED 91, got {rc} {st.get('exit')}")
            check(st.get("exit", {}).get("last_pc") == "$8000",
                  f"{machine}: expected the trap at $8000, got {st.get('exit', {}).get('last_pc')}")
            check(st.get("mhz") == st0.get("mhz"),
                  f"{machine}: clock changed by a nextreg ({st.get('mhz')} vs {st0.get('mhz')} for DI;HALT)")
            mode = dm.get("memory", {}).get("scld_mode")
            check(mode == dm0.get("memory", {}).get("scld_mode"),
                  f"{machine}: SCLD mode changed by nextreg $69: {mode}")

        _, st0, dm0 = run(inspeccy, ctl, "tc2048", work, ["--lenient-undoc-ed"])
        rc, st, dm = run(inspeccy, sna, "tc2048", work, ["--lenient-undoc-ed"])
        check(rc == 6, f"tc2048 lenient: expected exit 6 (DI;HALT), got {rc} {st.get('exit')}")
        check(st.get("mhz") == st0.get("mhz"), f"tc2048 lenient: clock changed: {st.get('mhz')}")
        check(dm.get("memory", {}).get("scld_mode") == dm0.get("memory", {}).get("scld_mode"),
              f"tc2048 lenient: SCLD mode changed: {dm.get('memory', {}).get('scld_mode')}")
        # on top of the control's DI;HALT: 2 x (8T undocumented-ED nop) + the
        # operand bytes executed as instructions: rlca 4, inc bc 6, ld l,c 4,
        # ld (bc),a 7 = 37 T
        base = st0.get("cycles", {}).get("executing", 0)
        got = st.get("cycles", {}).get("executing")
        check(got == base + 37,
              f"tc2048 lenient: expected {base} + 37 executing T-states, got {got}")

    for f in fails:
        print("FAIL: " + f)
    print("z80n_gate: " + ("ok" if not fails else f"{len(fails)} failure(s)"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
