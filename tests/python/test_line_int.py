"""Line interrupt (NR $22/$23) on next50.

Two asm programs, run via --dot (self-contained; no ROM needed — IM1 uses the
synthetic $0038 handler except in the position test, which installs its own):

  COUNTER (lineint.asm): DI, program $22/$23, IM1, EI, spin. Checked via the
  status interrupt counters. `accepted` is the TOTAL interrupts taken (frame +
  line); `line_accepted` is line-only; frame-only = accepted - line_accepted.
    R22=$00  ULA on, no line   -> accepted~=frames, line=0
    R22=$04  ULA masked, no line -> accepted=0, line=0     (bit2 masks the ULA int)
    R22=$06  line on, ULA off  -> line~=frames, frame-only~=0

  POSITION (lineint_pos.asm): maps RAM at $0000, installs a $0038 ISR that reads
  NR $1F (raster line) and di/HALTs if it is not within a few lines of the
  programmed line. So result=BudgetReached iff the line interrupt fired at the
  RIGHT line every frame; a wrong line -> HaltedInterruptsDisabled. Run for
  lines 40 / 100 / 250 (top / paper / lower border) to exercise the whole frame.

Skips (exit 77) without sjasmplus.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

HERE = Path(__file__).resolve().parents[1]
ASM_ROOT = HERE / "asm"
TMP = HERE / "tmp"

COUNTER_ASM = """
	include "common.inc"
	IFNDEF R22
		DEFINE R22 $00
	ENDIF
	IFNDEF R23
		DEFINE R23 100
	ENDIF
	di
	ld sp, $BFFF
	set_nr $23, R23
	set_nr $22, R22
	im 1
	ei
.spin:
	jr .spin
"""

POS_ASM = """
	include "common.inc"
	IFNDEF LINE
		DEFINE LINE 100
	ENDIF
	di
	ld sp, $BFFF
	set_nr $50, 0			; RAM bank-0 low 8K over $0000
	ld hl, Handler
	ld de, $0038
	ld bc, HandlerEnd - Handler
	ldir
	set_nr $23, LINE
	set_nr $22, $06			; line-int on, ULA masked
	im 1
	ei
.spin:
	jr .spin
Handler:
	push af
	push bc
	ld a, $1F
	ld bc, $243B
	out (c), a
	ld bc, $253B
	in a, (c)
	cp LINE - 1
	jr c, .bad
	cp LINE + 3
	jr nc, .bad
	pop bc
	pop af
	ei
	reti
.bad:
	di
	halt
HandlerEnd:
"""

# HW-vector mode (NR $C0 bit 0): the line interrupt is source index 0. LINEEN =
# NR $C4 (bit 1 enables the line int in HW-vector mode). The define is NOT named
# C4 - that token collides with the register literal $C4 under -D substitution.
HW_ASM = """
	include "common.inc"
	IFNDEF LINEEN
		DEFINE LINEEN $02
	ENDIF
	di
	ld sp, $BFFF
	ld hl, LHandler			; IM2 table: source 0 vector byte = base($A0)|0 = $A0,
	ld ($A0A0), hl			; so the handler address is read from ($A0<<8)|$A0.
	ld a, $A0
	ld i, a
	set_nr $C0, $A1			; base $A0 + HW-vector enable (bit 0)
	set_nr $C4, LINEEN		; bit 1 = line-int enable in HW-vector mode
	set_nr $23, 100
	set_nr $22, $06			; line-int on, ULA masked
	im 2
	ei
.spin:
	jr .spin
LHandler:
	ei
	reti
"""


def locate_inspeccy():
    return locate_test_binary()


def assemble(src: str, defines: dict[str, int], name: str) -> Path | None:
    asm = TMP / f"{name}.asm"
    asm.write_text(src, encoding="utf-8")
    dot = TMP / f"{name}.dot"
    cmd = ["sjasmplus", f"-i{ASM_ROOT}"]
    cmd += [f"-D{k}={v}" for k, v in defines.items()]
    cmd += [f"--raw={dot}", str(asm)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"line_int: sjasmplus failed for {name}\n{r.stdout}\n{r.stderr}")
        return None
    return dot


def run(inspeccy: Path, dot: Path, name: str) -> dict:
    status = TMP / f"{name}.json"
    r = subprocess.run([str(inspeccy), "--dot", str(dot), "--machine", "next50",
                        "--until-frame", "60", "--no-bmp", "--no-dump",
                        "--status-out", str(status), "--out", str(TMP / f"{name}.out.sna")],
                       capture_output=True, text=True)
    out = {"summary": (r.stdout + r.stderr)}
    if status.is_file():
        out["status"] = json.loads(status.read_text(encoding="utf-8"))
    return out


def main() -> int:
    inspeccy = locate_inspeccy()
    if inspeccy is None or not inspeccy.is_file():
        print("line_int: SKIP (no inspeccy binary)")
        return 77
    if shutil.which("sjasmplus") is None:
        print("line_int: SKIP (sjasmplus not on PATH)")
        return 77
    TMP.mkdir(exist_ok=True)
    ok = True

    # --- Counter cases ------------------------------------------------------
    cases = {
        "ula_on":     (0x00, 0),   # frame ints only
        "ula_masked": (0x04, 0),   # nothing (bit2 masks ULA, no line)
        "line_on":    (0x06, 1),   # line ints; frame masked
    }
    for name, (r22, want_line) in cases.items():
        dot = assemble(COUNTER_ASM, {"R22": r22, "R23": 100}, f"li_{name}")
        if dot is None:
            return 1
        res = run(inspeccy, dot, f"li_{name}")
        it = res.get("status", {}).get("interrupts")
        if it is None:
            print(f"line_int: FAIL {name} (no status)")
            return 1
        accepted = it["accepted"]
        line     = it["line_accepted"]
        frame_only = accepted - line
        if name == "ula_on":
            if not (line == 0 and accepted >= 55):
                print(f"line_int: FAIL ula_on: accepted={accepted} line={line} (want line 0, accepted>=55)")
                ok = False
        elif name == "ula_masked":
            if not (accepted == 0 and line == 0):
                print(f"line_int: FAIL ula_masked: accepted={accepted} line={line} (want both 0 — ULA masked)")
                ok = False
        else:  # line_on
            if not (line >= 55 and frame_only <= 2 and it["line"] == 100):
                print(f"line_int: FAIL line_on: accepted={accepted} line={line} "
                      f"frame_only={frame_only} line#={it['line']} "
                      f"(want line>=55, frame_only<=2, line#=100)")
                ok = False

    # --- HW-vector mode (NR $C0 bit 0): line = source 0, gated by $C4 bit 1 ---
    for lineen, want in ((0x02, True), (0x00, False)):
        dot = assemble(HW_ASM, {"LINEEN": lineen}, f"lh_{lineen}")
        if dot is None:
            return 1
        res = run(inspeccy, dot, f"lh_{lineen}")
        it = res.get("status", {}).get("interrupts", {})
        line = it.get("line_accepted", 0)
        if want and line < 55:
            print(f"line_int: FAIL hw-vector enabled ($C4=2): line_accepted={line} (want >=55)")
            ok = False
        if (not want) and line != 0:
            print(f"line_int: FAIL hw-vector disabled ($C4=0): line_accepted={line} (want 0 — $C4 bit1 gate)")
            ok = False

    # --- Position cases -----------------------------------------------------
    for L in (40, 100, 250):
        dot = assemble(POS_ASM, {"LINE": L}, f"lp_{L}")
        if dot is None:
            return 1
        res = run(inspeccy, dot, f"lp_{L}")
        summ = res["summary"]
        if "result=BudgetReached" not in summ:
            print(f"line_int: FAIL position line {L}: the $0038 ISR sampled a "
                  f"raster line off the programmed one (result != BudgetReached)\n  {summ.strip()[:200]}")
            ok = False

    print("line_int: ok" if ok else "line_int: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
