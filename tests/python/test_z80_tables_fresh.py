#!/usr/bin/env python3
"""Check that the bundled opcode reference regenerates the committed header."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parents[1]
INSPECCY_DIR = TESTS_DIR.parent
GEN = INSPECCY_DIR / "tools" / "gen_z80_tables.py"
TABLE = INSPECCY_DIR / "tests" / "refs" / "z80_fingerprint" / "next50.json"


def main() -> int:
    if not TABLE.is_file():
        print(f"FAIL: bundled opcode reference not found at {TABLE}")
        return 1
    p = subprocess.run([sys.executable, str(GEN), "--check"], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        print("z80_tables_fresh: STALE (run tools/gen_z80_tables.py)")
        return 1
    print("z80_tables_fresh: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
