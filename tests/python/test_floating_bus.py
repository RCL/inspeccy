"""Classic 48K/128K floating bus (MemoryBus::FloatingBusByte).

A DI program paints all attributes $3F, one attribute row $28, then spins on
IN A,($FF) until it reads $28 - i.e. until the beam reaches that row's attribute
fetch. With the floating bus modelled the loop exits within the first frame and
reaches its HALT; without it (a constant $FF, e.g. on Pentagon) the loop spins
forever and the run only ends at the cycle budget.

Asserts: 48K exits via HALT well inside one frame; Pentagon (no floating bus)
does not. Skips without sjasmplus or the 48K ROM.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

HERE = Path(__file__).resolve().parents[1]
INSPECCY_DIR = HERE.parent
TMP = HERE / "tmp"

ASM = """
	device zxspectrum48
	org $8000
start:
	di
	ld sp, $bfff
	ld hl, $5800
	ld de, $5801
	ld bc, 767
	ld (hl), $3f
	ldir
	ld hl, $5940	; attr row 10 = $5800 + 10*32
	ld b, 32
	ld a, $28
r10:
	ld (hl), a
	inc hl
	djnz r10
	ld bc, $00ff
spin:
	in a, ($ff)
	cp $28
	jr nz, spin
	halt	; di + halt => haltedinterruptsdisabled
	savesna "fbtest.sna", start
"""


def locate_inspeccy():
    return locate_test_binary()


def locate_rom() -> Path | None:
    for c in (test_rom("48.rom"),):
        if c.is_file():
            return c
    return None


def run(inspeccy: Path, sna: Path, rom: Path, machine: str) -> str:
    p = subprocess.run([str(inspeccy), "--sna", str(sna), "--rom48", str(rom),
                        "--machine", machine, "--cycles", "2000000",
                        "--no-status", "--no-ay", "--no-bmp", "--no-dump",
                        "--out", str(TMP / "fb.out.sna")],
                       capture_output=True, text=True)
    return p.stdout + p.stderr


def field(out: str, key: str) -> str:
    for tok in out.split():
        if tok.startswith(key + "="):
            return tok[len(key) + 1:]
    return ""


def main() -> int:
    argv = sys.argv[1:]
    inspeccy = Path(argv[argv.index("--inspeccy") + 1]) if "--inspeccy" in argv else locate_inspeccy()
    if inspeccy is None or not inspeccy.is_file():
        print("floating_bus: SKIP (no inspeccy binary)")
        return 0
    if shutil.which("sjasmplus") is None:
        print("floating_bus: SKIP (sjasmplus not on PATH)")
        return 0
    rom = locate_rom()
    if rom is None:
        print("floating_bus: SKIP (no 48.rom image)")
        return 0
    TMP.mkdir(exist_ok=True)
    asm = TMP / "fbtest.asm"
    asm.write_text(ASM, encoding="utf-8")
    r = subprocess.run(["sjasmplus", asm.name], cwd=TMP, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"floating_bus: sjasmplus failed\n{r.stdout}\n{r.stderr}")
        return 1
    sna = TMP / "fbtest.sna"

    fails = []
    out48 = run(inspeccy, sna, rom, "48k")
    res48, cyc48 = field(out48, "result"), field(out48, "cycles")
    if res48 != "HaltedInterruptsDisabled":
        fails.append(f"48K: expected the floating-bus loop to exit into HALT, got result={res48}")
    elif int(cyc48 or 0) > 100000:
        fails.append(f"48K: exited but after {cyc48} cycles (> ~1.4 frames); beam timing looks off")

    outp = run(inspeccy, sna, rom, "pentagon")
    resp = field(outp, "result")
    if resp != "BudgetReached":
        fails.append(f"Pentagon: expected the loop to spin (no floating bus), got result={resp}")

    if fails:
        print("floating_bus: FAIL")
        for f in fails:
            print("  " + f)
        return 1
    print("floating_bus: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
