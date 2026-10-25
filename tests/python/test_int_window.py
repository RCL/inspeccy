#!/usr/bin/env python3
"""Bounded ULA /INT window (hardware behaviour).

The ULA asserts /INT for ~32 T at the frame top and drops it. A Z80 with IFF1=0
for that whole pulse - an ISR still running, a DI section, Timex INTDISABLE -
never sees that frame's interrupt: the frame is DROPPED and the next chance is
the next frame top. inspeccy used to keep the request pending and take it the
moment IFF1 came back, even mid-frame, which hid every overrunning ISR.

Synthesises a 48K IM 2 program whose ISR burns a chosen number of cycles, then
EI; RETI, with a main loop of `jr $`. Runs 20 frames (48K: 69888 T each).

The first pulse comes at the END of frame 0 (the deadline is armed one period
ahead), so 20 frames carry 19 pulses.
  ISR ~90k T (> one frame):
    default (window): every other pulse taken -> ints == 10, missed == 9
    --int-window latch (legacy): taken late whenever EI -> ~15 ints, missed == 0
  ISR ~30k T (fits): ints == 19, missed == 0 in both modes.

Also checks the interrupt phase from a trace: with the window, every accepted
interrupt lands within the pulse of a frame top; with latch, the long-ISR run
takes some mid-frame.

Exit 0 = pass, 1 = fail, 77 = prerequisite (ROM) missing.
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

TESTS_DIR = Path(__file__).resolve().parents[1]
INSPECCY_DIR = TESTS_DIR.parent
ROM48 = test_rom("48.rom")

FRAME = 69888
FRAMES = 20
ORG = 0x8000
ISR = 0x8484          # vector: table bytes are $84 -> $8484
TABLE_HI = 0x81       # I = $81 -> table at $8100..$8200 (257 bytes)


def make_sna(path: Path, isr_iterations: int) -> None:
    ram = bytearray(49152)

    def put(addr, data):
        ram[addr - 0x4000:addr - 0x4000 + len(data)] = data

    # main: im 2 ; ld a,$81 ; ld i,a ; ei ; loop: jr loop
    put(ORG, bytes([0xED, 0x5E, 0x3E, TABLE_HI, 0xED, 0x47, 0xFB, 0x18, 0xFE]))
    put(0x8100, bytes([0x84]) * 257)
    # isr: push bc ; ld bc,N ; l: dec bc ; ld a,b ; or c ; jr nz,l ; pop bc ; ei ; reti
    #   loop body = 6 + 4 + 4 + 12 = 26 T per iteration
    n = isr_iterations
    put(ISR, bytes([0xC5, 0x01, n & 0xFF, n >> 8, 0x0B, 0x78, 0xB1, 0x20, 0xFB, 0xC1, 0xFB, 0xED, 0x4D]))
    sp = 0xFF00
    hdr = struct.pack("<BHHHHHHHHHBBHHBB", TABLE_HI, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x04, 0, 0, sp - 2, 2, 7)
    ram[sp - 2 - 0x4000] = ORG & 0xFF
    ram[sp - 1 - 0x4000] = ORG >> 8
    path.write_bytes(hdr + bytes(ram))


def run(inspeccy: str, sna: Path, work: Path, extra: list[str], trace: bool = False):
    tr = work / "t.txt"
    cmd = [inspeccy, "--sna", str(sna), "--machine", "48k", "--rom48", str(ROM48),
           "--cycles", str(FRAME * FRAMES), "--no-ay", "--no-bmp", "--no-status", "--no-dump"] + extra
    if trace:
        cmd += ["--trace-out", str(tr), "--trace-format", "verbose"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = p.stdout + p.stderr
    m_i = re.search(r"\bints=(\d+)", out)
    m_m = re.search(r"\bmissed=(\d+)", out)
    if not (m_i and m_m):
        raise RuntimeError(out[-400:])
    phases = []
    if trace and tr.is_file():
        for line in tr.read_text(encoding="utf-8", errors="replace").splitlines():
            if "Interrupt, vector=" in line:
                m = re.match(r"\s*(\d+)", line)
                if m:
                    phases.append(int(m.group(1)) % FRAME)
    return int(m_i.group(1)), int(m_m.group(1)), phases


def main() -> int:
    inspeccy = str(locate_test_binary())
    a = sys.argv[1:]
    if "--inspeccy" in a:
        inspeccy = a[a.index("--inspeccy") + 1]
    if not inspeccy:
        for c in (INSPECCY_DIR / "build" / "inspeccy" / "Release" / "inspeccy.exe",
                  INSPECCY_DIR / "build" / "inspeccy" / "inspeccy"):
            if c.is_file():
                inspeccy = str(c); break
    if not inspeccy:
        print("inspeccy binary not found"); return 1
    if not ROM48.is_file():
        print(f"SKIP: missing {ROM48}"); return 77

    fails: list[str] = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    with tempfile.TemporaryDirectory(prefix="intwin_") as td:
        work = Path(td)
        long_sna = work / "long.sna"; make_sna(long_sna, 3400)    # ~88.4k + overhead > 1 frame
        short_sna = work / "short.sna"; make_sna(short_sna, 1100)  # ~28.6k, fits

        # --- overrunning ISR, hardware window: every other frame, the rest dropped
        ints, missed, phases = run(inspeccy, long_sna, work, [], trace=True)
        check(ints == 10, f"window/long: expected 10 interrupts (every other pulse), got {ints}")
        check(missed == 9, f"window/long: expected 9 missed, got {missed}")
        if phases:
            bad = [p for p in phases if p >= 64]
            check(not bad, f"window/long: {len(bad)}/{len(phases)} interrupts accepted mid-frame (phase >= 64 T): {bad[:4]}")
            print(f"  window/long: {len(phases)} interrupt(s) in trace, max phase {max(phases)} T into the frame")

        # --- same ISR, legacy latch: taken late, mid-frame, nothing reported missed
        ints_l, missed_l, phases_l = run(inspeccy, long_sna, work, ["--int-window", "latch"], trace=True)
        check(ints_l > 12, f"latch/long: legacy model should take the interrupt late (~15), got {ints_l}")
        check(missed_l == 0, f"latch/long: legacy model never reports misses, got {missed_l}")
        if phases_l:
            check(any(p >= 64 for p in phases_l), "latch/long: expected some mid-frame acceptances under the legacy model")

        # --- ISR that fits: identical in both modes, nothing missed
        for label, extra in (("window", []), ("latch", ["--int-window", "latch"])):
            i2, m2, _ = run(inspeccy, short_sna, work, extra)
            check(i2 == FRAMES - 1, f"{label}/short: expected {FRAMES - 1} interrupts, got {i2}")
            check(m2 == 0, f"{label}/short: expected 0 missed, got {m2}")

    for f in fails:
        print("FAIL: " + f)
    print("int_window: " + ("ok" if not fails else f"{len(fails)} failure(s)"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
