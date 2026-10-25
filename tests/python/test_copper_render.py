#!/usr/bin/env python3
"""Copper render oracle: copper build == software build, pixel for pixel.

For each technique we ship two self-contained ZX Next programs that must render
to the SAME static image:

  * the *copper* build drives the effect with the Copper VM + per-scanline
    NextReg writes, rendered through the --pixel-accurate scanline compositor;
  * the *software* build reaches the identical image by hand (pre-shaped
    framebuffer, constant registers), rendered through the normal path.

The harness assembles both with sjasmplus, runs each under inspeccy with
--bmp-out, and asserts the two BMPs are byte-identical. A mismatch means the
copper execution or the pixel-accurate sampling drifted from the ground truth
(wrong scanline phase, off-by-one line/column, collapsed per-line effect, ...).

Techniques:
  scale -- vertical scanline scaling: copper rewrites NR $33 per line (x4) vs a
           pre-doubled 96-row software source (x2 hardware).
  dup   -- left-side duplication: copper resets NR $32 mid-scanline (sub-line X)
           vs a pre-duplicated software source.
  page  -- mid-frame Layer-2 page flip: copper switches NR $12 between two static
           buffers (tear at row 96) vs a hand-split single-buffer software image.

Caveat: this proves inspeccy's copper path == its software path (self-consistent),
NOT that either matches real ZX Next hardware. Hardware validation (register latch
granularity, copper H-compare timing, the exact display geometry) needs the Next
VHDL or a hardware capture.

Exit codes: 0 = all pass, 1 = a mismatch or tooling failure (details on stderr).
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


from test_support import binary_candidates, locate_test_binary, test_rom

HERE         = Path(__file__).resolve().parents[1] / "data" / "copper"           # tests/data/copper
TESTS_DIR    = HERE.parents[1]                               # tests
INSPECCY_DIR = TESTS_DIR.parent                          # project root
ASM_DIR      = TESTS_DIR / "asm"                         # for common.inc
ROM = test_rom("128.rom")

# No ROM images ship with the tree; skip (exit 77) when absent.
if not ROM.is_file():
    print(f"SKIP: ROM image not present ({ROM})")
    raise SystemExit(77)

BIN_CANDIDATES = binary_candidates()

# (name, copper.asm, software.asm) -- the copper build renders --pixel-accurate.
TECHNIQUES = [
    ("scale", "scale_copper.asm", "scale_soft.asm"),
    ("dup",   "dup_copper.asm",   "dup_soft.asm"),
    ("page",  "page_copper.asm",  "page_soft.asm"),
]


def find_inspeccy():
    return locate_test_binary()


def assemble(sjasmplus: str, asm: Path, out_dot: Path) -> None:
    proc = subprocess.run(
        [sjasmplus, f"-i{ASM_DIR}", f"-i{HERE}", f"--raw={out_dot}", str(asm)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout.decode("utf-8", "replace"))
        sys.exit(f"sjasmplus failed on {asm.name} (exit {proc.returncode})")


def render(inspeccy: Path, dot: Path, bmp: Path, pixel_accurate: bool) -> None:
    cmd = [str(inspeccy), "--dot", str(dot),
           "--rom48", str(ROM), "--rom128", str(ROM),
           "--machine", "next50", "--seconds", "1",
           "--no-status", "--no-dump", "--bmp-out", str(bmp)]
    if pixel_accurate:
        cmd.append("--pixel-accurate")
    else:
        # The software twin is the static baked baseline; render it with the
        # plain end-of-frame snapshot, not the frame-exact replay (which is now
        # the default --bmp-out). The oracle is: copper build's beam-timed frame
        # == the twin's static image.
        cmd += ["--bmp-exact", "off"]
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    # PASS -> exit 5 (RestartTrap); anything else is a run failure worth showing.
    if proc.returncode != 5:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
        sys.exit(f"inspeccy exit {proc.returncode} for {dot.name} "
                 f"(expected 5/RestartTrap)")
    if not bmp.is_file():
        sys.exit(f"no BMP produced at {bmp}")


def diff_bmps(copper: Path, soft: Path) -> tuple[int, str]:
    a, b = copper.read_bytes(), soft.read_bytes()
    if len(a) != len(b):
        return 1, f"size {len(a)} (copper) != {len(b)} (software)"
    diffs = [i for i in range(len(a)) if a[i] != b[i]]
    if not diffs:
        return 0, ""
    sample = ", ".join(
        f"@{i}: copper=0x{a[i]:02X} soft=0x{b[i]:02X}" for i in diffs[:6])
    return len(diffs), f"{len(diffs)} bytes differ; first: {sample}"


def main() -> int:
    inspeccy = find_inspeccy()
    sjasmplus = shutil.which("sjasmplus")
    if not sjasmplus:
        sys.exit("sjasmplus not on PATH")
    if not ROM.is_file():
        sys.exit(f"ROM not found: {ROM}")

    failures = 0
    with tempfile.TemporaryDirectory(prefix="copper_render_") as td:
        td = Path(td)
        for name, cop_asm, soft_asm in TECHNIQUES:
            cop_dot,  soft_dot  = td / f"{name}_cop.dot",  td / f"{name}_soft.dot"
            cop_bmp,  soft_bmp  = td / f"{name}_cop.bmp",  td / f"{name}_soft.bmp"

            assemble(sjasmplus, HERE / cop_asm,  cop_dot)
            assemble(sjasmplus, HERE / soft_asm, soft_dot)
            render(inspeccy, cop_dot,  cop_bmp,  pixel_accurate=True)
            render(inspeccy, soft_dot, soft_bmp, pixel_accurate=False)

            n, msg = diff_bmps(cop_bmp, soft_bmp)
            if n == 0:
                print(f"PASS: {name} -- copper == software ({cop_bmp.stat().st_size} bytes)")
            else:
                failures += 1
                sys.stderr.write(f"FAIL: {name} -- {msg}\n")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
