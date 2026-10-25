#!/usr/bin/env python3
"""Check that the bundled opcode table regenerates the sjasmplus fork's effects header."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parents[1]
INSPECCY_DIR = TESTS_DIR.parent
GEN = INSPECCY_DIR / "tools" / "gen_z80_effects.py"
HEADER = os.environ.get("INSPECCY_EFFECTS_HEADER")


def main() -> int:
    if not HEADER:
        print("SKIP: set INSPECCY_EFFECTS_HEADER to the external generated header")
        return 77
    p = subprocess.run([sys.executable, str(GEN), "--out", HEADER, "--check"], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        print("z80_effects_fresh: STALE (run tools/gen_z80_effects.py)")
        return 1
    print("z80_effects_fresh: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
