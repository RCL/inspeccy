#!/usr/bin/env python3
"""--cycle-exact (default on) frame-locks --video-out to the real frame period.

With cycle-exact on, one composite BMP is emitted per raster frame at the
frame-interrupt phase, and the reported fps is the machine's true refresh
(processor clock / frame period) - not a wall-clock fps that drifts against
the raster. With --cycle-exact off, the old behaviour returns: a fixed
50/60 fps cadence that is not the machine's refresh.

Checked on 48K (frame period 69888 T at 3.5 MHz = 50.0801 Hz true refresh):
  - on : stderr says "frame-locked", ~one BMP per emulated frame
  - off: stderr says the integer fps, BMP count follows the 50 fps cadence

Exit 0 = pass, 1 = fail, 77 = prerequisite (ROM/fixture) missing.
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

FRAMES = 120
TRUE_FPS_48K = 3_500_000 / 69888               # 50.0801...
ORG = 0x8000


def make_sna(path: Path) -> None:
    """A 48K snapshot running  EI; loop: HALT; JR loop  in IM 1 - one frame
    interrupt (and HALT wake) per frame."""
    ram = bytearray(49152)
    code = bytes([0xFB, 0x76, 0x18, 0xFD])     # ei ; halt ; jr -1 (back to halt)
    ram[ORG - 0x4000:ORG - 0x4000 + len(code)] = code
    sp = 0xFF00
    ram[sp - 2 - 0x4000] = (ORG + 1) & 0xFF     # after EI, at the HALT
    ram[sp - 1 - 0x4000] = (ORG + 1) >> 8
    # I, HL', DE', BC', AF', HL, DE, BC, IY, IX, IFF2(bit2), R, AF, SP, IM, border
    hdr = struct.pack("<BHHHHHHHHHBBHHBB", 0x3F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x04, 0, 0, sp - 2, 1, 7)
    path.write_bytes(hdr + bytes(ram))


def run(inspeccy: str, work: Path, sna: Path, extra: list[str]) -> tuple[str, int]:
    out = work / "f%05d.bmp"
    for f in work.glob("*.bmp"):
        f.unlink()
    cmd = [inspeccy, "--sna", str(sna), "--machine", "48k", "--rom48", str(ROM48),
           "--until-frame", str(FRAMES), "--video-out", str(out),
           "--no-status", "--no-dump", "--no-bmp"] + extra
    p = subprocess.run(cmd, capture_output=True, text=True)
    m = re.search(r"videoframes=(\d+)", p.stdout + p.stderr)
    nframes = int(m.group(1)) if m else len(list(work.glob("*.bmp")))
    return (p.stdout + p.stderr), nframes


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

    with tempfile.TemporaryDirectory(prefix="ce_video_") as td:
        work = Path(td)
        sna = work / "eihalt.sna"
        make_sna(sna)

        log_on, n_on = run(inspeccy, work, sna, [])                       # cycle-exact defaults on
        check("frame-locked" in log_on, f"on: expected 'frame-locked' in the video line, got: "
              f"{[l for l in log_on.splitlines() if l.startswith('video:')]}")
        m = re.search(r"@ ([\d.]+) fps \(frame-locked", log_on)
        check(m is not None and abs(float(m.group(1)) - TRUE_FPS_48K) < 0.01,
              f"on: expected the true 48K refresh {TRUE_FPS_48K:.4f} fps, got {m.group(1) if m else '?'}")
        # frame-locked: one BMP per raster frame over the window (allow +/-1 for the boundary)
        check(abs(n_on - FRAMES) <= 1, f"on: expected ~{FRAMES} frame-locked BMPs, got {n_on}")

        log_off, n_off = run(inspeccy, work, sna, ["--cycle-exact", "off"])
        check("frame-locked" not in log_off, "off: video should NOT be frame-locked")
        check(re.search(r"@ 50 fps", log_off) is not None,
              f"off: expected the fixed 50 fps cadence, got: "
              f"{[l for l in log_off.splitlines() if l.startswith('video:')]}")

    for f in fails:
        print("FAIL: " + f)
    print("cycle_exact_video: " + ("ok" if not fails else f"{len(fails)} failure(s)"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
