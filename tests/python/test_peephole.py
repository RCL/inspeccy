#!/usr/bin/env python3
"""Peephole reporter regression test (--optimize-out).

Covers signed cost arithmetic and self-modifying operands:

  1. A rewrite that saves a byte but COSTS T-states was reported as a speed
     win, and its negative saving wrapped through unsigned arithmetic into
     ~4.29e9 T per hit -- which then dominated every report.

  2. Self-modifying code read as constants. The sjasmplus idiom

         .Done equ $+1
             ld a,0

     makes that operand a variable; "a already holds $00, drop it" would have
     silently broken the program. Three such traps were reported. Such findings
     are now STRUCK at report time -- not flagged -- because with an exact
     (page, offset) write key the finding is not merely fragile, it is wrong.

Both are checked with hand-built fixtures where the right answer is known
exactly, rather than by eyeballing a demo's report.

Needs sjasmplus on PATH; skips (77) without it.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

TESTS_DIR = Path(__file__).resolve().parents[1]
INSPECCY_DIR = TESTS_DIR.parent
TMP = TESTS_DIR / "tmp"


def locate_inspeccy():
    return locate_test_binary()


# Three things at once:
#   $8007  `ld a,0` whose operand is poked   -> must be struck
#   $800E  an identical `ld a,0`, never poked -> must survive
#   $A00E  a write at the SAME offset-within-page as $800E but in a different
#          8K page. An offset-only write key would strike $800E too; the 24-bit
#          (page, offset) key must tell them apart. This is the paging case.
SMC_ASM = """
	device zxspectrumnext
	org $8000
start:
	di
	ld sp, $bfff
	xor a
	ld b, 50
loop:
smcoperand equ $ + 1
	ld a, 0
	ld hl, smcoperand
	ld (hl), 0
	ld a, 0
	ld hl, $a00e
	ld (hl), 0
	djnz loop
spin:
	jr spin
	savenex open "smctest.nex", start, $bfff
	savenex core 3, 0, 0
	savenex cfg 0
	savenex auto
	savenex close
"""

# +2 on a pair is 12T/2B against 10T/3B: a byte saved, two T-states lost.
# +1 is 6T/1B against 10T/3B: a win on both.
# $10FF -> $1101 is chosen so NEITHER half matches: otherwise the half-pair
# rule wins the site (3T/1B beats -2T/1B) and this would not test inc x2 at all.
PROF_ASM = """
	device zxspectrumnext
	org $8000
start:
	di
	ld sp, $bfff
	ld b, 20
loop:
	ld hl, $10ff
	ld hl, $1101
	ld de, $2000
	ld de, $2001
	djnz loop
spin:
	jr spin
	savenex open "proftest.nex", start, $bfff
	savenex core 3, 0, 0
	savenex cfg 0
	savenex auto
	savenex close
"""


# Half-pair loads, from both directions.
#   A) value-based: D already holds the high half, so only E needs loading.
#   B) per-byte SMC: only the LOW operand byte is ever poked, so the high half
#      is a constant being reloaded every pass -- `ld de,FOO` that wants to be
#      `ld e,BAR`. This is what per-byte (rather than per-instruction) write
#      tracking buys: per-instruction can only say "self-modified, discard".
HALF_ASM = """
	device zxspectrumnext
	org $8000
start:
	di
	ld sp, $bfff
	ld b, 30
loop:
	ld de, $1200
	ld de, $1234
delowoperand equ $ + 1
	ld de, $5678
	ld hl, delowoperand
	ld (hl), $78
	djnz loop
spin:
	jr spin
	savenex open "halftest.nex", start, $bfff
	savenex core 3, 0, 0
	savenex cfg 0
	savenex auto
	savenex close
"""


# `inc rr` -> `inc <lo>` soundness across an UNROLLED walk.
#   A) four `inc hl` reloaded each pass, never leaving the $90xx page -> all
#      safe, and they are one edit, so they must collapse to a single row.
#   B) four `inc de` starting at $8FFE, so the walk crosses $8FFF -> $9000 in
#      the middle of the chain. Only the second one actually carries; the other
#      three look clean in isolation, which is exactly why per-site analysis is
#      not enough -- nobody converts three of four identical lines.
CARRY_ASM = """
	device zxspectrumnext
	org $8000
start:
	di
	ld sp, $bfff
	ld b, 30
loop:
	ld hl, $9000
	inc hl
	inc hl
	inc hl
	inc hl
	ld de, $8ffe
	inc de
	inc de
	inc de
	inc de
	djnz loop
spin:
	jr spin
	savenex open "carrytest.nex", start, $bfff
	savenex core 3, 0, 0
	savenex cfg 0
	savenex auto
	savenex close
"""


def build(name: str, source: str) -> Path | None:
    asm = TMP / f"{name}.asm"
    asm.write_text(source, encoding="utf-8")
    r = subprocess.run(["sjasmplus", asm.name], cwd=TMP,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"sjasmplus failed for {name}:\n{r.stdout}\n{r.stderr}")
        return None
    nex = TMP / f"{name}.nex"
    return nex if nex.is_file() else None


def report(inspeccy: Path, nex: Path, out: Path, profile: str = "all") -> list[str]:
    subprocess.run([str(inspeccy), "--nex", str(nex), "--machine", "next50",
                    "--cycles", "200000", "--optimize-out", str(out),
                    "--optimize-profile", profile, "--no-status", "--no-dump"],
                   capture_output=True, text=True)
    if not out.is_file():
        return []
    lines = out.read_text(encoding="utf-8").splitlines()
    # The table only. The report also LISTS struck findings further down, in a
    # similar shape, so stop at the blank line that ends the table -- otherwise
    # a suppressed finding reads as a surviving one.
    rows: list[str] = []
    seen = False
    for ln in lines:
        if not seen:
            if ln.startswith("site "):
                seen = True
            continue
        if not ln.strip() or ln.startswith("struck as unsafe"):
            break
        rows.append(ln)
    return rows


def struck_section(out: Path) -> list[str]:
    """The 'struck as unsafe' listing, which must name what it suppressed."""
    lines = out.read_text(encoding="utf-8").splitlines()
    for i, ln in enumerate(lines):
        if ln.startswith("struck as unsafe"):
            return lines[i:]
    return []


def main() -> int:
    inspeccy = locate_inspeccy()
    if inspeccy is None:
        print("SKIP: inspeccy binary not built")
        return 77
    if shutil.which("sjasmplus") is None:
        print("SKIP: sjasmplus not on PATH")
        return 77
    TMP.mkdir(parents=True, exist_ok=True)

    # --- 1. the self-modified site is struck, the identical one survives ----
    # Both sites carry the same suggestion text, so a report that keeps exactly
    # one of them is proof the write-watch discriminates by address rather than
    # by pattern. The kept one must be the site nothing writes to.
    nex = build("smctest", SMC_ASM)
    if nex is None:
        return 1
    out = TMP / "peep_smc.txt"
    rows = [r for r in report(inspeccy, nex, out) if "ld a,$00" in r]
    if len(rows) != 1:
        print(f"FAIL: expected exactly 1 surviving 'ld a,$00' finding, got {len(rows)}")
        for r in rows:
            print("   ", r)
        return 1
    # $8007 is the poked one, $800E the clean one (see SMC_ASM). $800E also
    # shares its offset-within-page with the $A00E write, so its survival is
    # what proves the write key carries the page and not just the offset.
    if "$800E" not in rows[0]:
        print(f"FAIL: the surviving finding is not the clean site: {rows[0]}")
        print("      (if it is missing entirely, the write key ignored the page "
              "and $A00E struck $800E)")
        return 1
    # The suppressed one must be NAMED, with the byte that condemned it -- a
    # silent removal is indistinguishable from "nothing found here", which is
    # exactly how a whole self-modifying program once read as clean.
    struck = struck_section(out)
    if not any("$8007" in r for r in struck):
        print("FAIL: the struck finding is not named in the report")
        for r in struck:
            print("   ", r)
        return 1
    if not any("$8008" in r for r in struck):
        print("FAIL: the struck listing does not say WHICH byte was written "
              "($8008, the operand)")
        for r in struck:
            print("   ", r)
        return 1

    # --- 1b. loader writes are not self-modification -------------------------
    # Every byte of a program delivered by a loader is "written during the run".
    # Counting that as self-modification struck every finding in the program
    # under test while leaving the ROM's intact -- the report then said the
    # program was clean. Only writes from a site's FIRST EXECUTION onwards
    # count. Both `ld a,$00` sites here are written by nothing before they run,
    # so the earlier assertions already cover the negative; this checks the
    # positive case that the struck one names an OPERAND byte ($8008), not the
    # opcode byte ($8007) a loader would have touched.
    if any(" $8007 written" in r for r in struck):
        print("FAIL: struck on the opcode byte, which is the loader's signature, "
              "not self-modification")
        return 1

    # --- 2. a size win that costs T-states never appears under `speed` -------
    nex = build("proftest", PROF_ASM)
    if nex is None:
        return 1

    speed = report(inspeccy, nex, TMP / "peep_speed.txt", "speed")
    if any("inc hl x2" in r for r in speed):
        print("FAIL: 'inc hl x2' (2 T-states SLOWER) shown under --optimize-profile speed")
        return 1
    if not any("inc de" in r for r in speed):
        print("FAIL: 'inc de' (a genuine speed win) missing from the speed profile")
        return 1

    size = report(inspeccy, nex, TMP / "peep_size.txt", "size")
    x2 = [r for r in size if "inc hl x2" in r]
    if not x2:
        print("FAIL: 'inc hl x2' (a 1-byte win) missing from the size profile")
        return 1
    # The T column must be NEGATIVE, not a wrapped ~4.29e9.
    m = re.search(r"\s(-?\d+)\s+\d+\s+ld hl", x2[0])
    if m is None:
        print(f"FAIL: could not parse the T column from: {x2[0]}")
        return 1
    saved = int(m.group(1))
    if saved >= 0:
        print(f"FAIL: 'inc hl x2' reports {saved} T saved; it costs 2 T per hit, "
              f"so this must be negative (unsigned wrap regression)")
        return 1

    # --- 3. half-pair loads, value-based and per-byte-SMC-based -------------
    nex = build("halftest", HALF_ASM)
    if nex is None:
        return 1
    rows = report(inspeccy, nex, TMP / "peep_half.txt")

    # A: high half already correct -> load only the low register.
    val = [r for r in rows if "ld de,$1234" in r and "ld e,$34" in r]
    if not val:
        print("FAIL: value-based half-pair load not reported "
              "(d already holds $12, so only e needs loading)")
        for r in rows:
            print("   ", r)
        return 1

    # B: only the low operand byte is poked, so the high half is a constant.
    # A per-INSTRUCTION write check could only discard this site; per-byte turns
    # it into an actionable finding.
    smc_half = [r for r in rows if "only the LOW half is poked" in r]
    if not smc_half:
        print("FAIL: half-poked immediate not reported; per-byte write tracking "
              "should see that only the low operand byte changes")
        for r in rows:
            print("   ", r)
        return 1
    if "$56" not in smc_half[0]:
        print(f"FAIL: constant high half misidentified: {smc_half[0]}")
        return 1

    # --- 4. inc rr -> inc <lo>: sound only where the walk never carries ------
    nex = build("carrytest", CARRY_ASM)
    if nex is None:
        return 1
    out = TMP / "peep_carry.txt"
    rows = [r for r in report(inspeccy, nex, out) if "inc " in r and "->" in r]

    hl = [r for r in rows if "inc hl" in r]
    de = [r for r in rows if "inc de" in r]
    if len(hl) != 1:
        print(f"FAIL: the safe 4x 'inc hl' chain should be ONE merged row, got {len(hl)}")
        for r in rows:
            print("   ", r)
        return 1
    if "x4 unrolled" not in hl[0]:
        print(f"FAIL: the safe chain was not collapsed: {hl[0]}")
        return 1
    if "100%" not in hl[0]:
        print(f"FAIL: merged row's held% is wrong (hits summed but not runs): {hl[0]}")
        return 1
    if de:
        print("FAIL: the 'inc de' chain crosses a page boundary mid-walk and must "
              "be struck entirely, including the members that never carry:")
        for r in de:
            print("   ", r)
        return 1
    if "crossed a byte" not in out.read_text(encoding="utf-8"):
        print("FAIL: the carry-struck finding was removed without saying so")
        return 1

    print(f"PASS: self-modified site struck (clean twin kept); size-only "
          f"rewrite excluded from speed, reported as {saved} T under size; "
          f"half-pair loads found both by value and by per-byte SMC; "
          f"unrolled inc-walk merged when safe, struck whole when it carries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
