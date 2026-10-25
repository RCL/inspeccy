#!/usr/bin/env python3
"""Benchmark the bundled synthetic workload; extra flags pass to perf_benchmark.py.

Use perf_benchmark.py directly to measure your own program.
"""
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
if __name__ == "__main__":
    raise SystemExit(subprocess.call([sys.executable, str(HERE / "perf_benchmark.py"),
                                     str(HERE.parent / "tests/data/nex/trace_workload.nex"), *sys.argv[1:]]))
