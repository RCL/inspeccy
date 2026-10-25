#!/usr/bin/env python3
"""Measure headless execution with and without binary tracing.

Times include process startup and input loading. Audio and BMP output are off.
Binary trace bytes go to the platform null device. CSV results identify the
executable by SHA-256; they are local measurements, not performance guarantees.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time
from snapshot_periodic import locate_binary, positive

ROOT = Path(__file__).resolve().parent.parent


def seconds(text):
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="Input .nex, .dot, .sna or .z80")
    parser.add_argument("--inspeccy")
    parser.add_argument("--machine", default="next50")
    parser.add_argument("--rom48128", type=Path)
    parser.add_argument("--emulated-seconds", type=seconds, default=10)
    parser.add_argument("--repetitions", type=positive, default=3)
    parser.add_argument("--csv", type=Path, default=ROOT / "tests/tmp/perf_results.csv")
    parser.add_argument("--copper-compare", type=int, default=0, metavar="N",
                        help="Measure N interleaved copper-on/off pairs instead of writing CSV")
    args = parser.parse_args()
    if args.copper_compare < 0:
        parser.error("--copper-compare must be non-negative")
    binary = locate_binary(args.inspeccy)
    source = args.binary.resolve()
    flag = {".nex": "--nex", ".dot": "--dot", ".sna": "--sna", ".z80": "--z80"}.get(source.suffix.lower())
    if not flag or not source.is_file():
        parser.error("input must be an existing .nex, .dot, .sna or .z80")
    csv_path = args.csv.resolve()
    for protected in (source, binary, args.rom48128):
        if protected and (csv_path == protected.resolve() or
                          (csv_path.exists() and protected.exists() and csv_path.samefile(protected))):
            parser.error("CSV output aliases an input or the executable")
    common = [str(binary), flag, str(source), "--machine", args.machine,
              "--seconds", str(args.emulated_seconds), "--no-ay", "--no-status", "--no-dump", "--no-bmp"]
    if args.rom48128:
        common += ["--rom48128", str(args.rom48128.resolve())]
    with tempfile.TemporaryDirectory(prefix="inspeccy-perf-") as temporary:
        common += ["--out", str(Path(temporary) / "unused.sna")]

        def measure(trace=False, no_copper=False):
            command = list(common)
            if trace:
                command += ["--trace-out-bin", os.devnull, "--no-trace-index"]
            if no_copper:
                command.append("--no-copper")
            start = time.perf_counter()
            result = subprocess.run(command, capture_output=True, text=True)
            elapsed = time.perf_counter() - start
            if result.returncode:
                raise RuntimeError(f"emulator exited {result.returncode}: {result.stderr}")
            return elapsed

        if args.copper_compare:
            differences = []
            for _ in range(args.copper_compare):
                enabled, disabled = measure(), measure(no_copper=True)
                differences.append(100 * (enabled - disabled) / disabled)
            print(f"Copper on/off median difference: {statistics.median(differences):+.2f}% "
                  f"({len(differences)} pairs; includes timing noise)")
            return 0
        no_trace, trace = [], []
        for _ in range(args.repetitions):
            no_trace.append(measure())
            trace.append(measure(trace=True))
    row = {
        "ExecutableSHA256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "OS": platform.system() + " " + platform.release(),
        "CPU": platform.processor() or platform.machine(),
        "Input": source.name,
        "InputSHA256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "Machine": args.machine,
        "EmulatedSeconds": args.emulated_seconds,
        "Repetitions": args.repetitions,
        "MedianNoTraceSeconds": f"{statistics.median(no_trace):.6f}",
        "MedianTraceSeconds": f"{statistics.median(trace):.6f}",
    }
    existing = csv_path.is_file() and csv_path.stat().st_size > 0
    if existing:
        with csv_path.open(newline="", encoding="utf-8") as stream:
            if next(csv.reader(stream), None) != list(row):
                parser.error("CSV header differs; choose a new output file")
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("a", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(row))
        if not existing:
            writer.writeheader()
        writer.writerow(row)
    print(f"{source.name}: no trace {row['MedianNoTraceSeconds']}s, trace {row['MedianTraceSeconds']}s -> {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
