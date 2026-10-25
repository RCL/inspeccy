#!/usr/bin/env python3
"""Render periodic screenshots by replaying the original input at each timestamp.

This preserves peripherals that JSON dump/resume does not serialize. Work grows
with the sum of the requested timestamps. VFS mounts are read-only so repeated
runs cannot change their input files. Existing outputs require --force.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def positive(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def locate_binary(override):
    if override:
        candidates = [Path(override)]
    elif os.environ.get("INSPECCY"):
        candidates = [Path(os.environ["INSPECCY"])]
    else:
        candidates = [ROOT / folder / name
                      for folder in ("build/inspeccy/Release", "build/inspeccy", "build/Release", "build", "bin")
                      for name in ("inspeccy.exe", "inspeccy")]
        if shutil.which("inspeccy"):
            candidates.append(Path(shutil.which("inspeccy")))
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("Build inspeccy or provide --inspeccy / INSPECCY")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--seconds-total", type=positive, default=70)
    parser.add_argument("--interval-seconds", type=positive, default=1)
    parser.add_argument("--machine", default="next50")
    parser.add_argument("--out-dir", type=Path, default=Path("snapshots"))
    parser.add_argument("--inspeccy")
    parser.add_argument("--dot-arg", default="")
    parser.add_argument("--vfs", type=Path)
    parser.add_argument("--rom48128", type=Path)
    parser.add_argument("--rom48", type=Path)
    parser.add_argument("--rom128", type=Path)
    parser.add_argument("--rom-plus3", type=Path)
    parser.add_argument("--force", action="store_true", help="Replace the exact outputs for the requested timestamps")
    args = parser.parse_args(argv)
    source = args.source.resolve()
    source_flag = {".nex": "--nex", ".dot": "--dot", ".sna": "--sna", ".z80": "--z80"}.get(source.suffix.lower())
    if not source.is_file() or not source_flag:
        parser.error("--source must name an existing .nex, .dot, .sna or .z80 file")
    if args.interval_seconds > args.seconds_total:
        parser.error("--interval-seconds exceeds --seconds-total")
    binary = locate_binary(args.inspeccy)
    destination = args.out_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    common = [str(binary), source_flag, str(source), "--machine", args.machine, "--no-status"]
    for option in ("rom48128", "rom48", "rom128", "rom_plus3"):
        value = getattr(args, option)
        if value:
            common += ["--" + option.replace("_", "-"), str(value.resolve())]
    if args.dot_arg:
        common += ["--dot-arg", args.dot_arg]
    if args.vfs:
        common += ["--vfs", str(args.vfs.resolve()), "--vfs-readonly"]
    timestamps = range(args.interval_seconds, args.seconds_total + 1, args.interval_seconds)
    suffixes = (".bmp", ".dump.json", ".log")
    protected = [source, binary]
    protected += [value.resolve() for value in
                  (args.rom48128, args.rom48, args.rom128, args.rom_plus3) if value]
    for second in timestamps:
        for suffix in suffixes:
            output = destination / f"{source.stem}_t{second:04d}s{suffix}"
            for input_path in protected:
                same = output.resolve() == input_path
                if output.exists() and input_path.exists():
                    same = same or output.samefile(input_path)
                if same:
                    parser.error(f"output aliases an input: {output}")
    if not args.force:
        for second in timestamps:
            for suffix in suffixes:
                path = destination / f"{source.stem}_t{second:04d}s{suffix}"
                if path.exists():
                    parser.error(f"output already exists: {path}; use --force to replace it")
    for second in timestamps:
        name = f"{source.stem}_t{second:04d}s"
        with tempfile.TemporaryDirectory(prefix=".snapshot-", dir=destination) as temporary:
            work = Path(temporary)
            bmp = work / "frame.bmp"
            # Dump references are relative to its final directory.
            dump = destination / (name + ".dump.json")
            command = [*common, "--seconds", str(second), "--out", str(work / "out.sna"),
                       "--bmp-out", str(bmp), "--dump-out", str(dump)]
            result = subprocess.run(command, capture_output=True, text=True)
            log = destination / (name + ".log")
            temporary_log = work / "run.log"
            temporary_log.write_text(result.stdout + result.stderr, encoding="utf-8")
            temporary_log.replace(log)
            if result.returncode not in (0, 5, 6) or not bmp.is_file() or not dump.is_file():
                print(f"Snapshot at {second}s failed ({result.returncode}); see {log}")
                return 1
            # The dump describes this run; it is retained for inspection only.
            bmp.replace(destination / (name + ".bmp"))
            print(f"{second}s: {destination / (name + '.bmp')}")
            if result.returncode in (5, 6):
                print("Program terminated before the requested timestamp.")
                break
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
