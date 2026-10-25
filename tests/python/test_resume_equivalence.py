"""--dump-out / --resume must be invisible: a run split in two by a dump and a
resume ends exactly where the same run done in one go ends.

For each case the program runs T2 cycles straight through, and again as T1
cycles, a dump, and a resume for the remaining T2 - T1. Compared at the end:
  * the final dumps, field by field (only the timestamp, the build string and
    file paths may differ);
  * the final frame (--bmp-out);
  * the audio (--audio-out): the straight WAV against the two halves joined;
  * the PSG capture where the case records one.
Any difference is a piece of machine state the dump does not carry or the
resume does not restore - the class of bug this test exists to keep at zero.

Cases cover synthetic NEX workloads, VFS operations, video, audio and coverage.
Tape and disk boot checks use generated BASIC programs and user-supplied ROMs.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

from test_support import locate_test_binary, test_rom
from boot_fixture import write_tap, write_trd

HERE = Path(__file__).resolve().parents[1]
NEX = HERE / "data" / "nex"
TMP = HERE / "tmp" / "resume_equivalence"
NEXT_FRAME = 559104  # one 50 Hz frame at 28 MHz
NEXT_FRAME_14 = 279552  # and at 14 MHz
CLASSIC_FRAME = 70908  # one 128K frame at 3.5469 MHz
IGNORED_KEYS = {"created_at_utc", "tool_version", "wall_ms"}
PATH_KEYS = {"path", "out_path", "root_path", "opened_as", "resume_path"}


def kitchen_sink():
    """Assemble the synthetic workload; None without sjasmplus."""
    if shutil.which("sjasmplus") is None:
        return None
    TMP.mkdir(parents=True, exist_ok=True)
    source = HERE / "data" / "resume" / "kitchen_sink.asm"
    shutil.copy(source, TMP / source.name)
    r = subprocess.run(["sjasmplus", source.name], cwd=TMP, capture_output=True, text=True)
    return TMP / "kitchen_sink.nex" if r.returncode == 0 else None


def cases():
    programs = HERE / "data" / "programs"
    out = {
        "nullform": (["--nex", str(programs / "NullForm.nex")], 300 * NEXT_FRAME, 457 * NEXT_FRAME + 12345, False),
        "touching_the_sun": (["--nex", str(programs / "TouchingTheSun.nex"), "--ts"], 300 * NEXT_FRAME,
                             451 * NEXT_FRAME + 777, True),
        "rtzx": (["--nex", str(programs / "rtzx_main_next.nex")], 200 * NEXT_FRAME, 330 * NEXT_FRAME + 5, True),
        "snow": (["--nex", str(programs / "snownononext.nex")], 150 * NEXT_FRAME, 260 * NEXT_FRAME + 77, False),
        "sup": (["--nex", str(programs / "sup_main.nex")], 150 * NEXT_FRAME, 260 * NEXT_FRAME + 3, True),
        # split while a file is open for writing, open for reading, and while a
        # directory listing is half read
        "esxdos_writing": (["--nex", str(NEX / "esxdos_rw.nex")], 150, 1300, False),
        "esxdos_reading": (["--nex", str(NEX / "esxdos_rw.nex")], 400, 1300, False),
        "esxdos_listing": (["--nex", str(NEX / "esxdos_rw.nex")], 1000, 1300, False),
        "workload": (["--nex", str(NEX / "trace_workload.nex")], 50 * NEXT_FRAME, 120 * NEXT_FRAME + 11, True),
    }
    sink = kitchen_sink()
    if sink is not None:
        # split mid-frame; recorded with video and coverage, which must join up
        out["kitchen_sink"] = (["--nex", str(sink), "--ts"], 40 * NEXT_FRAME_14 + 12345, 90 * NEXT_FRAME_14 + 77,
                               True, {"video": True, "coverage": True})
        # after the line interrupt: the CTC sits between its control word and time constant
        out["kitchen_sink_odd"] = (["--nex", str(sink), "--ts"], 41 * NEXT_FRAME_14 + 200000,
                                   90 * NEXT_FRAME_14 + 77, True, {})
    tape, disk = TMP / "boot.tap", TMP / "boot.trd"
    write_tap(tape)
    write_trd(disk, "BOOTTEST")
    rom48 = test_rom("48.rom")
    if rom48.is_file():
        # the contended 48K: contention and the floating bus across the split
        out["tape_48k"] = (["--tap", str(tape), "--machine", "48k",
                            "--rom48", str(rom48)], 400 * 69888 + 5, 900 * 69888 + 31, True)
    rom128, trdos = test_rom("128.rom"), test_rom("trdos504t.rom")
    if rom128.is_file():
        out["tape_128k"] = (["--tap", str(tape), "--machine", "128k",
                             "--rom48128", str(rom128)], 400 * CLASSIC_FRAME, 900 * CLASSIC_FRAME + 17, True)
        if trdos.is_file():
            out["trdos_boot"] = (["--trd", str(disk), "--trd-run", "BOOTTEST",
                                  "--machine", "128k", "--rom48128", str(rom128), "--rom-trdos", str(trdos)],
                                 500 * CLASSIC_FRAME, 1100 * CLASSIC_FRAME + 9, True)
    return out


def diff(a, b, path="", out=None, limit=40):
    """Field-by-field differences between two dumps."""
    if out is None:
        out = []
    if len(out) >= limit:
        return out
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key in IGNORED_KEYS:
                continue
            if key not in a or key not in b:
                out.append(f"{path}.{key}: only in the {'straight' if key in a else 'resumed'} run")
            elif key in PATH_KEYS and isinstance(a[key], str) and isinstance(b[key], str):
                if Path(a[key]).name != Path(b[key]).name:
                    out.append(f"{path}.{key}: {a[key]} != {b[key]}")
            else:
                diff(a[key], b[key], f"{path}.{key}", out, limit)
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)):
            diff(x, y, f"{path}[{i}]", out, limit)
    elif a != b:
        out.append(f"{path}: {str(a)[:70]} != {str(b)[:70]}")
    return out


def wav_data(path: Path):
    if not path.is_file():
        return None
    blob = path.read_bytes()
    at = blob.find(b"data")
    return blob[at + 8:] if at >= 0 else None


def run(inspeccy: Path, args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run([str(inspeccy), "--no-status", *args], capture_output=True, text=True)


def check(inspeccy: Path, name: str, args: list[str], t1: int, t2: int, psg: bool, extra: dict) -> list[str]:
    folder = TMP / name
    shutil.rmtree(folder, ignore_errors=True)
    straight, split = folder / "straight", folder / "split"
    straight.mkdir(parents=True)
    split.mkdir(parents=True)
    # each run's filesystem is its own folder, apart from what the test writes
    # (a program that lists its directory would see the dumps otherwise)
    (straight / "vfs").mkdir()
    (split / "vfs").mkdir()
    for f in extra.get("vfs_files", []):
        shutil.copy(f, straight / "vfs")
        shutil.copy(f, split / "vfs")
    outputs = lambda d, tag: ["--vfs", str(d / "vfs"), "--dump-out", str(d / f"{tag}.json"), "--bmp-out",
                              str(d / f"{tag}.bmp"), "--audio-out", str(d / f"{tag}.wav")] + \
        (["--psg-out", str(d / f"{tag}.psg")] if psg else []) + \
        (["--video-out", str(d / "frames" / "f%06d.bmp")] if extra.get("video") else []) + \
        (["--coverage-out", str(d / f"{tag}.cov")] if extra.get("coverage") else [])
    a = run(inspeccy, [*args, "--cycles", str(t2), *outputs(straight, "final")])
    m = run(inspeccy, [*args, "--cycles", str(t1), *outputs(split, "mid")])
    # the first half stops on an instruction boundary a few T-states past t1;
    # a resumed budget counts from there, so aim it at the straight run's end
    try:
        reached = json.loads((split / "mid.json").read_text())["stats"]["cycles_total"]
    except (OSError, ValueError, KeyError) as e:
        return [f"no mid dump: {e}"]
    b = run(inspeccy, ["--resume", str(split / "mid.json"), "--cycles", str(t2 - reached),
                       *outputs(split, "final")])
    problems = []
    for label, r in (("straight", a), ("first half", m), ("resumed half", b)):
        if r.returncode not in (0, 2, 6):
            problems.append(f"{label} run exited {r.returncode}: {(r.stdout + r.stderr).strip()[-300:]}")
    if a.returncode != b.returncode:
        problems.append(f"exit codes differ: straight {a.returncode}, resumed {b.returncode}")
    if problems:
        return problems
    try:
        dump_a = json.loads((straight / "final.json").read_text())
        dump_b = json.loads((split / "final.json").read_text())
    except (OSError, ValueError) as e:
        return [f"no final dump: {e}"]
    problems += diff(dump_a, dump_b)
    if (straight / "final.bmp").read_bytes() != (split / "final.bmp").read_bytes():
        problems.append("final frame differs")
    whole, first, rest = wav_data(straight / "final.wav"), wav_data(split / "mid.wav"), wav_data(split / "final.wav")
    if whole is not None and first is not None and rest is not None:
        joined = first + rest
        if whole != joined:
            at = next((i for i in range(min(len(whole), len(joined))) if whole[i] != joined[i]), None)
            problems.append(f"audio differs: {len(whole)} B straight vs {len(first)}+{len(rest)} B split, "
                            f"first difference at byte {at} (split at {len(first)})")
    if psg and (straight / "final.psg").read_bytes() != (split / "final.psg").read_bytes():
        problems.append("PSG capture differs")
    if extra.get("video"):
        frames = sorted(p.name for p in (straight / "frames").glob("*.bmp"))
        joined = sorted(p.name for p in (split / "frames").glob("*.bmp"))
        if frames != joined:
            problems.append(f"video frames differ in number or naming: {len(frames)} straight, {len(joined)} split")
        else:
            bad = [f for f in frames if (straight / "frames" / f).read_bytes() != (split / "frames" / f).read_bytes()]
            if bad:
                problems.append(f"{len(bad)} video frames differ, first {bad[0]}")
    if extra.get("coverage") and (straight / "final.cov").read_bytes() != (split / "final.cov").read_bytes():
        problems.append("coverage map differs")
    return problems


def main() -> int:
    argv = sys.argv[1:]
    inspeccy = Path(argv[argv.index("--inspeccy") + 1]) if "--inspeccy" in argv else locate_test_binary()
    only = [a for i, a in enumerate(argv) if not a.startswith("--") and (i == 0 or argv[i - 1] != "--inspeccy")]
    TMP.mkdir(parents=True, exist_ok=True)
    failed = False
    for name, case in cases().items():
        if only and name not in only:
            continue
        args, t1, t2, psg = case[:4]
        problems = check(inspeccy, name, args, t1, t2, psg, case[4] if len(case) > 4 else {})
        if problems:
            failed = True
            print(f"resume_equivalence: {name}: FAIL")
            for p in problems:
                print("    " + p)
        else:
            print(f"resume_equivalence: {name}: ok")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
