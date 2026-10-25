#!/usr/bin/env python3
"""Run an input once and collect a dump, BMP and optional text trace.

The trace queue buffers writes; it does not limit the size of the trace file.
"""
import argparse
from pathlib import Path
import subprocess
from snapshot_periodic import locate_binary, positive


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--seconds", type=positive, default=30)
    parser.add_argument("--machine", default="next50")
    parser.add_argument("--trace-queue-mb", type=positive, default=16)
    parser.add_argument("--no-trace", action="store_true")
    parser.add_argument("--no-bmp", action="store_true")
    parser.add_argument("--inspeccy")
    parser.add_argument("--out-dir", type=Path, default=Path("run-output"))
    parser.add_argument("--rom48128", type=Path)
    parser.add_argument("--rom48", type=Path)
    parser.add_argument("--rom128", type=Path)
    parser.add_argument("--rom-plus3", type=Path)
    options = parser.parse_args(argv)
    if options.trace_queue_mb > 1024:
        parser.error("--trace-queue-mb must be between 1 and 1024")
    source = options.source.resolve()
    flag = {".nex": "--nex", ".dot": "--dot", ".sna": "--sna", ".z80": "--z80"}.get(source.suffix.lower())
    if not source.is_file() or not flag:
        parser.error("source must be an existing .nex, .dot, .sna or .z80 file")
    folder = options.out_dir.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    stem = folder / f"{source.stem}_{options.seconds}s"
    command = [str(locate_binary(options.inspeccy)), flag, str(source), "--machine", options.machine,
               "--seconds", str(options.seconds), "--no-status", "--out", str(stem) + ".sna",
               "--dump-out", str(stem) + ".dump.json"]
    for option in ("rom48128", "rom48", "rom128", "rom_plus3"):
        value = getattr(options, option)
        if value:
            command += ["--" + option.replace("_", "-"), str(value.resolve())]
    command += ["--no-bmp"] if options.no_bmp else ["--bmp-out", str(stem) + ".bmp"]
    if not options.no_trace:
        command += ["--trace-out", str(stem) + ".trace", "--trace-queue-mb", str(options.trace_queue_mb), "--trace-diff-only"]
    print(f"Outputs: {stem}.*", flush=True)
    return subprocess.run(command).returncode


if __name__ == "__main__":
    raise SystemExit(main())
