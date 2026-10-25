#!/usr/bin/env python3
"""Match native and decoded instruction/event records from the synthetic NEX.

Invocation banners differ; binary-only video annotations are excluded.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


from test_support import binary_candidates, locate_test_binary, test_rom

HERE         = Path(__file__).resolve().parents[1]
INSPECCY_DIR = HERE.parent
BIN_CANDIDATES = binary_candidates()
NEX = INSPECCY_DIR / "tests" / "data" / "nex" / "trace_workload.nex"
BUDGET_CYCLES = "200000"


def find_inspeccy():
    return locate_test_binary()


def run(*args: str) -> None:
    rc = subprocess.run(args, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL).returncode
    if rc != 0:
        sys.exit(f"inspeccy returned {rc} for: {' '.join(args)}")


def main() -> int:
    inspeccy = find_inspeccy()
    if not NEX.is_file():
        sys.exit(f"input NEX not found: {NEX}")

    with tempfile.TemporaryDirectory(prefix="btr_roundtrip_") as td:
        td = Path(td)
        native_path  = td / "native.txt"
        binary_path  = td / "binary.btr"
        decoded_path = td / "decoded.txt"

        run(str(inspeccy), "--machine", "next50", "--nex", str(NEX),
            "--cycles", BUDGET_CYCLES, "--trace-out", str(native_path),
            "--no-bmp", "--no-status", "--no-dump")

        run(str(inspeccy), "--machine", "next50", "--nex", str(NEX),
            "--cycles", BUDGET_CYCLES, "--trace-out-bin", str(binary_path),
            "--no-bmp", "--no-status", "--no-dump")

        with decoded_path.open("w") as f:
            rc = subprocess.run([str(inspeccy), "--trace-decode",
                                 str(binary_path)],
                                stdout=f).returncode
        if rc != 0:
            sys.exit(f"--trace-decode returned {rc}")

        # Read both. Drop the first line — the # invocation banner contains
        # the trace destination flag (--trace-out vs --trace-out-bin) and
        # is expected to differ. The "columns:" line and every record line
        # must match exactly. Binary-only video annotations have no native
        # text equivalent and are tested by the video-state tests.
        native_lines  = native_path.read_text().splitlines()[1:]
        decoded_lines = [line for line in decoded_path.read_text().splitlines()[1:]
                         if not (line.startswith("# ") and " video: " in line)]

        if native_lines == decoded_lines:
            print(f"PASS: {len(native_lines)} trace lines identical "
                  f"between native and decoded.")
            return 0

        # Report the first divergence — usually the most diagnostic.
        max_show = 3
        diffs_shown = 0
        n = max(len(native_lines), len(decoded_lines))
        sys.stderr.write(
            f"FAIL: native {len(native_lines)} lines, "
            f"decoded {len(decoded_lines)} lines — divergences:\n")
        for i in range(n):
            nl = native_lines[i]  if i < len(native_lines)  else "<missing>"
            dl = decoded_lines[i] if i < len(decoded_lines) else "<missing>"
            if nl != dl:
                sys.stderr.write(f"  line {i+2} (after header):\n")
                sys.stderr.write(f"    native:  {nl!r}\n")
                sys.stderr.write(f"    decoded: {dl!r}\n")
                diffs_shown += 1
                if diffs_shown >= max_show:
                    break
        return 1


if __name__ == "__main__":
    sys.exit(main())
