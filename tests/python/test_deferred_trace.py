"""Deferred recording follows emulated seconds through turbo transitions."""
import re
import struct
import subprocess
import tempfile
from pathlib import Path

from test_support import locate_test_binary


def main():
    executable = str(locate_test_binary())
    with tempfile.TemporaryDirectory(prefix="inspeccy-deferred-") as directory:
        work = Path(directory)
        header = bytearray(512)
        header[:8] = b"NextV1.2"
        header[9] = header[18 + 2] = 1
        struct.pack_into("<HH", header, 12, 0xBF00, 0x8000)

        def fixture(slow_down):
            # DI; NEXTREG 7,3 (28 MHz), optionally delay ~23 ms then 3.5 MHz.
            code = bytes.fromhex("F3 ED 91 07 03")
            if slow_down:
                code += bytes.fromhex("21 00 60 2B 7C B5 20 FB ED 91 07 00")
            code += b"\xC3" + struct.pack("<H", 0x8000 + len(code))
            path = work / "turbo.nex"
            path.write_bytes(header + code + bytes(16384 - len(code)))
            return path

        def run(source, timing, start, extra=()):
            trace = work / "run.btr"
            command = [executable, *source, *timing, *start, *extra,
                       "--trace-out-bin", str(trace), "--no-dump", "--no-bmp",
                       "--no-status", "--out", str(work / "out")]
            result = subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, (command, result.stdout, result.stderr)
            with trace.open("rb") as stream:
                fixed = stream.read(64)
            # The writer opens lazily; a wholly skipped run has an empty file.
            start_ns, end_ns, count = struct.unpack_from("<QQQ", fixed, 40) if fixed else (0, 0, 0)
            cycles = int(re.search(r"cycles=(\d+)", result.stdout)[1])
            return start_ns, end_ns, count, cycles

        for slow_down in (False, True):
            source = ["--nex", str(fixture(slow_down)), "--machine", "next50"]
            start, end, count, baseline_cycles = run(source, ["--seconds", ".05"], ["--trace-from-seconds", ".03"])
            assert count > 0 and abs(start - 30_000_000) < 10_000, (slow_down, start, count)
            assert abs(end - 50_000_000) < 10_000, end
            # A start beyond the duration must never extend the run.
            _, _, count, cycles = run(source, ["--seconds", ".05"], ["--trace-from-seconds", ".06"])
            assert count == 0 and abs(cycles - baseline_cycles) < 30, (cycles, count)
            # A fixed cycle stop still wins over a seconds-based trace start.
            _, _, count, cycles = run(source, ["--cycles", "400000"], ["--trace-from-seconds", ".03"])
            assert count == 0 and 400000 <= cycles < 400030, (cycles, count)

        # A fixed cycle start also needs clamping when a turbo slowdown moves
        # the overall seconds budget earlier than that start.
        _, _, count, cycles = run(source, ["--seconds", ".05"], ["--trace-from-cycles", "1000000"])
        assert count == 0 and abs(cycles - baseline_cycles) < 30, (cycles, count)
    print("PASS: deferred seconds across turbo up/down, seconds and cycle stop limits")


if __name__ == "__main__":
    main()
