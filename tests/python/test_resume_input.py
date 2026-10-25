#!/usr/bin/env python3
"""A resumed run driven by a new --input script presses its keys.

The dump carries the input it ran with; a resume used to put those events
back over whatever --input named, so a script written for the resumed run
was read and then dropped, with only a sha1 warning.  Now a different
script supplies the events from the dump's frame on, and the keys the dump
holds stay held.  The same script still resumes exactly as before.
"""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile

from test_support import locate_test_binary

TESTS = Path(__file__).resolve().parents[1]


def run(executable, options):
    result = subprocess.run([executable, *options, "--no-bmp", "--no-status"],
                            capture_output=True, text=True, timeout=30)
    if result.returncode not in (0, 2):
        raise RuntimeError(result.stdout + result.stderr)
    return result


def main() -> int:
    executable = str(locate_test_binary())
    if "--inspeccy" in sys.argv:
        executable = str(Path(sys.argv[sys.argv.index("--inspeccy") + 1]).resolve())
    nex = TESTS / "data/nex/trace_workload.nex"
    with tempfile.TemporaryDirectory(prefix="inspeccy_resume_input_") as directory:
        work = Path(directory)
        first = work / "first.txt"
        first.write_text("10 tap SPACE\n")
        run(executable, ["--machine", "next50", "--nex", str(nex), "--until-frame", "50",
                         "--input", str(first), "--dump-out", str(work / "a.json")])
        frame = json.loads((work / "a.json").read_text())
        frame = frame["stats"]["cycles_total"] // frame["scheduler"]["frame_int_period"]
        later = work / "later.txt"
        later.write_text(f"{frame - 5} press Q\n{frame + 5} press P\n")   # Q is in the past
        run(executable, ["--resume", str(work / "a.json"), "--until-frame", "20",
                         "--input", str(later), "--dump-out", str(work / "b.json")])
        rows = json.loads((work / "b.json").read_text())["input"]["keyboard_rows"]
        rows = [int(r.lstrip("$"), 16) for r in rows]
        p_down = not (rows[5] & 0x01)    # P: row 5 ($DF), bit 0
        q_down = not (rows[2] & 0x01)    # Q: row 2 ($FB), bit 0
        if not p_down or q_down:
            print(f"FAIL: keyboard rows after the resume {rows}: P down {p_down}, Q down {q_down}",
                  file=sys.stderr)
            return 1
        print("PASS: a new script drives the resumed run, from its frame on")
        same = run(executable, ["--resume", str(work / "a.json"), "--until-frame", "20",
                                "--input", str(first), "--dump-out", str(work / "c.json")])
        if "replace the dump's" in same.stderr:
            print("FAIL: the dump's own script was replaced by itself", file=sys.stderr)
            return 1
        print("PASS: the same script resumes as before")
    return 0


if __name__ == "__main__":
    sys.exit(main())
