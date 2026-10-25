"""WAIT-less copper pacing.

A copper list with NO WAITs, mode %10 (run, no per-frame reset): 1024 MOVEs to
NR $7F, value 1 for a wide window of the list and 0 elsewhere (the LAST entry is
0). The CPU runs with interrupts off and spins reading NR $7F, halting the moment
it reads 1.

On hardware the copper executes its MOVEs back-to-back forever, wrapping at the
list end, so NR $7F cycles and the spin exits. inspeccy used to give copper MOVEs
no execution time: a WAIT-less list piled every MOVE onto FrameOrigin in one
servicing (so NR $7F froze at the LAST value = 0) and then parked - the spin would
never see 1. With per-instruction pacing (copper.cpp), the MOVEs are spread over
time and NR $7F takes the value 1 across its window, so the CPU exits.

Asserts: the run exits via HALT (not the cycle budget). Skips without sjasmplus.
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
	device zxspectrumnext
	org $8000
start:
	di
	ld sp, $bfff
; ---- upload a wait-less copper list: 1024 moves to nr $7f ----
; nr $62=0 (stop, addr hi 0), nr $61=0 (addr lo 0): write cursor = 0.
	nextreg $62, $00
	nextreg $61, $00
	ld bc, $243b
	ld a, $60	; select nr $60 (copper data, 8-bit auto-inc)
	out (c), a
	ld bc, $253b
	dup 1024, i
	ld a, $7f	; move nr $7f  (hi byte, bit7=0 => move)
	out (c), a
	if (i >= 200) && (i < 900)
	ld a, 1
	else
	ld a, 0
	endif
	out (c), a	; value byte
	edup
; ---- run, mode %10 (no per-frame reset), from cpc 0 ----
	nextreg $61, $00
	nextreg $62, $80
; ---- spin on nr $7f until it reads 1 ----
	ld bc, $243b
	ld a, $7f
	out (c), a	; select nr $7f for the $253b read
	ld b, $25	; bc = $253b
spin:
	in a, (c)
	cp 1
	jr nz, spin
	halt	; di + halt => haltedinterruptsdisabled
	savenex open "waitless.nex", start, $bfff
	savenex core 3, 0, 0
	savenex cfg 0
	savenex auto
	savenex close
"""


def locate_inspeccy():
    return locate_test_binary()


def field(out: str, key: str) -> str:
    for tok in out.split():
        if tok.startswith(key + "="):
            return tok[len(key) + 1:]
    return ""


def main() -> int:
    argv = sys.argv[1:]
    inspeccy = Path(argv[argv.index("--inspeccy") + 1]) if "--inspeccy" in argv else locate_inspeccy()
    if inspeccy is None or not inspeccy.is_file():
        print("copper_waitless: SKIP (no inspeccy binary)")
        return 0
    if shutil.which("sjasmplus") is None:
        print("copper_waitless: SKIP (sjasmplus not on PATH)")
        return 0
    TMP.mkdir(exist_ok=True)
    asm = TMP / "waitless.asm"
    asm.write_text(ASM, encoding="utf-8")
    r = subprocess.run(["sjasmplus", asm.name], cwd=TMP, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"copper_waitless: sjasmplus failed\n{r.stdout}\n{r.stderr}")
        return 1
    nex = TMP / "waitless.nex"
    p = subprocess.run([str(inspeccy), "--nex", str(nex), "--machine", "next50",
                        "--cycles", "6000000", "--no-status", "--no-ay",
                        "--no-bmp", "--no-dump"],
                       capture_output=True, text=True)
    out = p.stdout + p.stderr
    res = field(out, "result")
    if res != "HaltedInterruptsDisabled":
        print("copper_waitless: FAIL")
        print(f"  expected the WAIT-less copper to pace NR $7F so the spin exits into HALT;")
        print(f"  got result={res} (a parked/unpaced copper freezes NR $7F and the spin never exits)")
        print(f"  cycles={field(out, 'cycles')} lastPC={field(out, 'lastPC')}")
        return 1
    print("copper_waitless: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
