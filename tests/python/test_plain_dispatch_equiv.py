#!/usr/bin/env python3
"""Compare general and specialised dispatch on reproducible RAM workloads,
and a traced run against both: tracing must not change what the machine does.

The snapshots exercise interrupts, RAM updates, bank switching and HALTs on
classic machines. A bundled NEX adds Next instructions and AY port traffic.
No original machine ROM is needed: all executed code and IM 2 vectors are RAM.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from test_support import locate_test_binary

TESTS = Path(__file__).resolve().parents[1]
VOLATILE = {"created_at_utc", "generated", "path", "source", "args", "wall_ms",
            "inspeccy", "written", "file", "timestamp"}


def ram_workload() -> bytes:
    ram = bytearray(49152)
    # DI; SP=$bffe; I=$82; IM 2; EI. All stack/vector/code accesses remain in RAM.
    code = bytearray.fromhex("f3 31 fe bf 3e 82 ed 47 ed 5e fb")
    loop = len(code)
    code += bytes.fromhex("76 21 00 90 06 40 34 23 10 fc")  # HALT; update 64 RAM bytes.
    code += bytes.fromhex("21 00 40 34")                    # Touch contended video RAM.
    code += bytes.fromhex("3a 00 90 e6 07 f6 10 01 fd 7f ed 79 32 00 c0")  # 7FFD bank select/write.
    code += bytes([0x18, (loop - len(code) - 2) & 255])
    ram[0x4000:0x4000 + len(code)] = code
    ram[0x4181:0x4181 + 10] = bytes.fromhex("f5 3a 00 91 3c 32 00 91 f1 fb")
    ram[0x418b:0x418d] = bytes.fromhex("ed 4d")             # RETI.
    ram[0x4200:0x4301] = bytes([0x81]) * 257
    return bytes(ram)


def snapshots(work: Path) -> tuple[Path, Path]:
    ram = ram_workload()
    header = bytearray(27)
    header[23:25] = struct.pack("<H", 0xbffc)
    header[25] = 2
    stacked = bytearray(ram)
    stacked[0x7ffc:0x7ffe] = b"\x00\x80"
    sna48 = work / "ram48.sna"
    sna48.write_bytes(header + stacked)
    sna128 = work / "ram128.sna"
    # The first 48K holds banks 5, 2 and 0; extension stores PC/7FFD/TR-DOS.
    rest = b"".join(bytes([bank]) * 16384 for bank in (1, 3, 4, 6, 7))
    sna128.write_bytes(header + ram + struct.pack("<HBB", 0x8000, 0x10, 0) + rest)
    return sna48, sna128


def digest(node):
    if isinstance(node, dict):
        return {key: digest(value) for key, value in sorted(node.items()) if key not in VOLATILE}
    if isinstance(node, list):
        return [digest(value) for value in node]
    if isinstance(node, str) and len(node) > 256:
        return hashlib.sha256(node.encode()).hexdigest()
    return node


def run(executable, options, dump):
    result = subprocess.run([executable, *options, "--no-ay", "--no-bmp", "--no-status",
                             "--dump-out", str(dump), "--out", str(dump.with_suffix(".sna"))],
                            capture_output=True, text=True, timeout=15)
    if result.returncode != 0 or not dump.is_file():
        raise RuntimeError(result.stdout + result.stderr)
    return digest(json.loads(dump.read_text(encoding="utf-8")))


def machine_state(digested):
    """What the machine did, without the record of how it was asked."""
    return {key: value for key, value in digested.items()
            if key not in ("run_options", "capture", "external_refs")}


def main() -> int:
    executable = str(locate_test_binary())
    if "--inspeccy" in sys.argv:
        executable = str(Path(sys.argv[sys.argv.index("--inspeccy") + 1]).resolve())
    with tempfile.TemporaryDirectory(prefix="inspeccy_dispatch_") as directory:
        work = Path(directory)
        sna48, sna128 = snapshots(work)
        rom = work / "unused.rom"
        rom.write_bytes(bytes(32768))
        cases = []
        for machine, snapshot in (("48k", sna48), ("128k", sna128), ("pentagon", sna128), ("ts2068", sna48)):
            for contention in ("off", "on"):
                options = ["--machine", machine, "--sna", str(snapshot), "--rom48", str(rom),
                           "--rom128", str(rom), "--cycles", "800000", "--contention", contention]
                cases.append((f"{machine}, contention {contention}", options))
        nex = TESTS / "data/nex/trace_workload.nex"
        for machine in ("next50", "next60"):
            cases.append((machine, ["--machine", machine, "--nex", str(nex), "--cycles", "800000"]))
        # at 28 MHz, where every memory read carries a wait state
        cases.append(("next50 at 28 MHz", ["--machine", "next50", "--nex",
                                           str(nex), "--mhz", "28", "--cycles", "3000000"]))
        for name, options in cases:
            plain = run(executable, options, work / "plain.json")
            full = run(executable, [*options, "--dispatch", "full"], work / "full.json")
            if plain != full:
                print(f"FAIL: dispatcher state differs for {name}", file=sys.stderr)
                return 1
            # The trace's own peeks at the opcode bytes once went through the
            # CPU's timed read, charging contention and Next wait states: a
            # traced run drifted a few T an instruction and a crash found
            # untraced did not happen under --trace-out.
            traced = run(executable, [*options, "--trace-out", str(work / "trace.txt")], work / "traced.json")
            if machine_state(plain) != machine_state(traced):
                print(f"FAIL: a traced run's state differs for {name}", file=sys.stderr)
                return 1
            print(f"PASS: {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
