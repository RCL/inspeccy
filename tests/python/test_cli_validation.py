#!/usr/bin/env python3
"""CLI rejection, malformed resume state, and HALT budget regressions."""

import copy
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_support import locate_test_binary


def main() -> int:
    executable = str(locate_test_binary())
    fixture = Path(__file__).parents[1] / "data/nex/trace_workload.nex"
    with tempfile.TemporaryDirectory(prefix="inspeccy_cli_") as directory:
        work = Path(directory)
        nex = work / "workload.nex"
        nex.write_bytes(fixture.read_bytes())
        common = ["--no-bmp", "--no-status", "--out", str(work / "out")]

        def run(options, expected):
            base_options = [item for item in common if not
                            (item == "--no-status" and "--status-out" in options) and not
                            (item == "--no-bmp" and "--bmp-out" in options)]
            result = subprocess.run([executable, *base_options, *options], cwd=work,
                                    capture_output=True, text=True, timeout=10)
            if result.returncode != expected:
                raise AssertionError(f"{options}: expected {expected}, got {result.returncode}\n{result.stderr}")
            return result

        assert subprocess.run([executable, "--help"], capture_output=True).returncode == 0
        for flag in ("--cycles", "--trace-from-cycles", "--frame-int", "--contention-x", "--audio-rate",
                     "--input-tap-frames", "--metrics-block", "--ram-mb", "--max-violations", "--video-fps", "--psg-rate"):
            for value in ("-1", "12junk", "18446744073709551616"):
                run(["--nex", str(nex), "--no-dump", flag, value], 4)
        for flag in ("--seconds", "--trace-from-seconds", "--until-frame", "--mhz", "--optimize-min-ratio"):
            for value in ("-1", "nan", "inf", "1.5junk", "1e9999"):
                run(["--nex", str(nex), "--no-dump", flag, value], 4)
        for flag, values in {
            "--until-pc": ("256:0", "1:65536", "1x:123", "0x1000000", "1:2:3"),
            "--cycle-exact": ("perhaps",), "--bmp-exact": ("perhaps",), "--dispatch": ("fast",),
            "--trace-queue-mb": ("0", "1025"),
            "--watch-phys": ("", "x:0", "1:12junk", "128:0", "1:4000", "1:d-1", "1:2:3"),
            "--coverage-reset-at-pc": ("", "10000", "12junk", "d-1", "18446744073709551616"),
            "--video-out": ("%s.bmp", "%n.bmp", "%d_%d.bmp", "%999999999d.bmp", "frame.bmp"),
            "--paper-out": ("%s.bmp", "%*d.bmp", "%"),
        }.items():
            for value in values:
                run(["--nex", str(nex), "--cycles", "3000", "--no-dump", flag, value], 4)

        script = work / "keys.txt"
        for text in ("1junk tap ENTER", "-1 press SPACE", "18446744073709551616 tap ENTER",
                     "0 tap 0 ENTER", "0 tap 2147483648 ENTER", "0 tap 2junk ENTER",
                     "0 press ENTER extra", "0 tap 2 ENTER extra",
                     "18446744073709551615 tap ENTER", "0 tap ENTER\0ignored"):
            script.write_text(text, encoding="ascii")
            run(["--nex", str(nex), "--cycles", "3000", "--no-dump", "--input", str(script)], 3)
        script.write_text("0 tap 2 ENTER\n4294967296 press SPACE\n", encoding="ascii")
        run(["--nex", str(nex), "--cycles", "3000", "--no-dump", "--input", str(script)], 0)

        invariants = work / "bounds.inv"
        for text, expected in (
            ("readonly8k = [255:1FFF-255:1FFF]", 0),
            ("readonly16k = [127:3FFF-127:3FFF]", 0),
            ("readonly8k = [256:0000-256:0000]", 3),
            ("readonly16k = [128:0000-128:0000]", 3),
            ("readonly8k = [65535:0000-65535:1FFF]", 3),
            ("readonly8k = [1:0000-0:0000]", 3),
            ("readonly8k = [255:1FFF-256:0000)", 0),
        ):
            invariants.write_text(text, encoding="ascii")
            run(["--nex", str(nex), "--cycles", "3000", "--no-dump",
                 "--invariants", str(invariants)], expected)
        invariants.write_bytes(b"#" + b" " * (16 * 1024 * 1024))
        run(["--nex", str(nex), "--cycles", "3000", "--no-dump",
             "--invariants", str(invariants)], 3)
        status = work / "breakpoint.json"
        run(["--nex", str(nex), "--until-pc", "0x8000", "--cycles", "3000",
             "--no-dump", "--status-out", str(status)], 8)
        assert json.loads(status.read_text(encoding="utf-8"))["exit"]["exit_code"] == 8

        blocked = work / "regular-file"
        blocked.write_bytes(b"not a directory")
        for flag, destination in (
            ("--dump-out", work), ("--status-out", work), ("--bmp-out", work),
            ("--coverage-out", work), ("--video-out", blocked / "%06d.bmp"),
            ("--paper-out", blocked / "%06d.bmp"),
        ):
            options = ["--nex", str(nex), "--cycles", "80000", flag, str(destination)]
            if flag != "--dump-out":
                options += ["--no-dump"]
            run(options, 3)
        run(["--nex", str(nex), "--cycles", "80000", "--no-dump",
             "--trace-out-bin", str(work / "trace.btr"), "--trace-index-out", str(work)], 3)

        original = nex.read_bytes()
        for flag in ("--dump-out", "--status-out", "--bmp-out", "--coverage-out", "--audio-out",
                     "--trace-out", "--trace-out-bin", "--psg-out", "--motion-out", "--histogram-out"):
            options = ["--nex", str(nex), "--cycles", "3000", flag, str(nex)]
            if flag != "--dump-out":
                options += ["--no-dump"]
            run(options, 4)
            assert nex.read_bytes() == original, flag + " overwrote the input"
        alias = work / "alias.nex"
        os.link(nex, alias)
        run(["--nex", str(nex), "--cycles", "3000", "--no-dump", "--trace-out", str(alias)], 4)
        assert nex.read_bytes() == original
        run(["--nex", str(nex), "--cycles", "3000", "--no-dump",
             "--trace-out-bin", str(work / "same.btr"), "--trace-index-out", str(work / "same.btr")], 4)
        run(["--nex", str(nex), "--cycles", "3000", "--no-dump", "--ts",
             "--psg-out", str(work / "capture.psg"), "--audio-out", str(work / "capture.2.psg")], 4)
        numbered = work / "frame000007.nex"
        numbered.write_bytes(original)
        run(["--nex", str(numbered), "--cycles", "3000", "--no-dump",
             "--video-out", str(work / "frame%06d.nex")], 4)
        assert numbered.read_bytes() == original
        assert not (work / "frame000000.nex").exists()
        run(["--nex", str(nex), "--cycles", "3000", "--no-dump",
             "--video-out", str(work / "frame%d.bmp"), "--paper-out", str(work / "frame1%d.bmp")], 4)
        # A generated name may be a hard link whose spelling does not match the input.
        linked_frame = work / "linked000001.bmp"
        os.link(nex, linked_frame)
        run(["--nex", str(nex), "--cycles", "180000", "--no-dump",
             "--video-out", str(work / "linked%06d.bmp")], 3)
        assert nex.read_bytes() == original, "frame output overwrote an input hard link"

        dump = work / "state.json"
        result = run(["--nex", str(nex), "--cycles", "3000", "--dump-out", str(dump)], 0)
        assert "cycles=3000 budget=3000 overshoot=0" in result.stdout
        state = json.loads(dump.read_text(encoding="utf-8"))
        run(["--resume", str(dump), "--cycles", "1000", "--no-dump"], 0)
        palette_state = copy.deepcopy(state)
        registers = bytearray(base64.b64decode(palette_state["next"]["nextregs_b64"]))
        registers[0x14] = 0x42
        palette_state["next"]["nextregs_b64"] = base64.b64encode(registers).decode("ascii")
        palette = bytes((0xFC, 1)) * 256
        palette_state["next"]["palettes"]["pal_0"] = base64.b64encode(palette).decode("ascii")
        dump.write_text(json.dumps(palette_state), encoding="utf-8")
        resumed_dump = work / "resumed.json"
        run(["--resume", str(dump), "--cycles", "1000", "--dump-out", str(resumed_dump)], 0)
        resumed = json.loads(resumed_dump.read_text(encoding="utf-8"))
        assert base64.b64decode(resumed["next"]["nextregs_b64"])[0x14] == 0x42
        assert base64.b64decode(resumed["next"]["palettes"]["pal_0"]) == palette

        # Paging bank 2/5 at C000 must emit each physical RAM bank exactly once.
        for current_bank in (2, 5):
            paged = copy.deepcopy(state)
            paged["memory"]["port_7ffd"] = "$" + f"{current_bank:02X}"
            paged["next"] = None
            paged["machine"] = "spectrum128k"
            paged["memory"]["kind"] = "spectrum128k"
            paged["nex"] = None
            stub_rom = work / "128.rom"
            stub_rom.write_bytes(bytes(32768))
            dump.write_text(json.dumps(paged), encoding="utf-8")
            run(["--resume", str(dump), "--rom48128", str(stub_rom), "--create-initial-dump",
                 "--dump-out", str(resumed_dump)], 0)
            pairs = []
            json.loads(resumed_dump.read_text(encoding="utf-8"),
                       object_pairs_hook=lambda items: pairs.extend(items) or dict(items))
            keys = [key for key, value in pairs if key.startswith("bank_")]
            assert sorted(keys) == [f"bank_{i}" for i in range(8)], keys

        # RAM size affects open-bus behavior and the Next allocator after resume.
        run(["--nex", str(nex), "--ram-mb", "1", "--cycles", "3000", "--dump-out", str(dump)], 0)
        assert json.loads(dump.read_text(encoding="utf-8"))["memory"]["ram_mb"] == 1
        for override, expected in (([], 1), (["--ram-mb", "2"], 2)):
            run(["--resume", str(dump), "--cycles", "1000", "--dump-out", str(resumed_dump), *override], 0)
            assert json.loads(resumed_dump.read_text(encoding="utf-8"))["memory"]["ram_mb"] == expected

        # +2A/+3 dumps need both paging latches and must retain their machine.
        rom_plus3 = work / "plus3.rom"
        rom_plus3.write_bytes(bytes(65536))
        snapshot = work / "plus3.sna"
        sna_data = bytearray((Path(__file__).parents[1] / "data" / "sna" / "restart_into_rom.sna").read_bytes())
        # RAM code selects ROM 3 through both ports and reads its identifying byte.
        code = bytes.fromhex("f3 31 ffbf 01 fd1f 3e04 ed79 01 fd7f 3e10 ed79 3a0000 320090 76")
        sna_data[27 + 16384:27 + 16384 + len(code)] = code
        sna_data[27 + 49152:27 + 49152 + 2] = bytes((0, 0x80))
        snapshot.write_bytes(sna_data)
        rom_plus3.write_bytes(b"".join(bytes((0x40 + i,)) * 16384 for i in range(4)))
        for model in ("+2a", "+3"):
            run(["--sna", str(snapshot), "--machine", model, "--rom-plus3", str(rom_plus3),
                 "--cycles", "3000", "--dump-out", str(resumed_dump)], 2)
            restored = json.loads(resumed_dump.read_text(encoding="utf-8"))
            bank2 = bytes.fromhex(" ".join(line.split(None, 1)[1] for line in restored["ram"]["bank_2"].splitlines()))
            assert bank2[0x1000] == 0x43, "snapshot load failed to enable +3 ROM paging"
            assert restored["memory"]["port_1ffd"] == "$04"

        for machine in ("spectrumplus2a", "spectrumplus3"):
            plus3_state = copy.deepcopy(state)
            plus3_state["machine"] = machine
            plus3_state["memory"]["kind"] = machine
            plus3_state["memory"]["port_7ffd"] = "$10"
            plus3_state["memory"]["port_1ffd"] = "$07"
            plus3_state["next"] = None
            plus3_state["nex"] = None
            # a state for another machine: none of the Next run's bus or slot table
            plus3_state.pop("bus", None)
            plus3_state["memory"].pop("base_slots", None)
            dump.write_text(json.dumps(plus3_state), encoding="utf-8")
            run(["--resume", str(dump), "--rom-plus3", str(rom_plus3), "--create-initial-dump",
                 "--dump-out", str(resumed_dump)], 0)
            restored = json.loads(resumed_dump.read_text(encoding="utf-8"))
            assert restored["machine"] == machine
            assert restored["memory"]["port_1ffd"] == "$07"
            assert restored["memory"]["port_7ffd"] == "$10"
        for key, value in (("machine", "unknown"), ("mhz", -1), ("mhz", "fast"),
                           ("contention_x", 4294967296), ("frame_int_period", -1)):
            changed = copy.deepcopy(state)
            changed[key] = value
            dump.write_text(json.dumps(changed), encoding="utf-8")
            run(["--resume", str(dump), "--cycles", "1000", "--no-dump"], 3)

        for group, key, value in (
            ("cpu", "PC", 65536), ("cpu", "AF", -1), ("cpu", "I", 256),
            ("cpu", "R", 1.5), ("cpu", "IM", 3), ("cpu", "IFF1", 2),
            ("cpu", "BC", "$10000000000000000"),
            ("scheduler", "next_frame_int_cycle", -1),
            ("scheduler", "next_event_cycle", 1.8446744073709552e19),
            ("stats", "cycles_total", -1), ("stats", "last_di_cycles", 1.5),
            ("memory", "ram_mb", 0), ("memory", "ram_mb", 3), ("memory", "ram_mb", "large"),
            ("memory", "border_colour", 8), ("memory", "port_7ffd", 256),
            ("memory", "paging_locked", "yes"), ("next", "palette_write_parity", 2),
            ("next", "last_selected_nextreg", 256), ("ay", "latch", 16),
        ):
            changed = copy.deepcopy(state)
            changed[group][key] = value
            dump.write_text(json.dumps(changed), encoding="utf-8")
            run(["--resume", str(dump), "--cycles", "1000", "--no-dump"], 3)
        for text in ("[" * 10000, '{"dump_version":1.}', '{"dump_version":1e}',
                     '{"dump_version":01}', '{"dump_version":18446744073709551616}'):
            dump.write_text(text, encoding="utf-8")
            run(["--resume", str(dump), "--cycles", "1000", "--no-dump"], 3)
    print("PASS: strict CLI values, safe filename patterns, dump ranges, output failures, and HALT cycle limit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
