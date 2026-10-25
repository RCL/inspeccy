#!/usr/bin/env python3
"""inspeccy test runner — cross-platform replacement for run_tests.ps1.

Two test surfaces:
  1) tests/cases/*.test       — declarative key/value tests graded against
                                 inspeccy's exit code + summary line + trace
                                 + output SNA/PSG/BMP/status/dump artifacts.
  2) tests/asm/**/*.asm       — assembly tests assembled via sjasmplus to dot
                                 commands, run via `inspeccy --dot`, graded by
                                 exit code (5 = pass via `jp $0000`, anything
                                 else = fail; the `di;nop;halt` FAIL macro
                                 produces exit 2). Tests under asm/video/
                                 additionally SHA1-diff their composite BMP
                                 against tests/refs/<same path>.bmp.

Usage:
    python tests/run_tests.py
    python tests/run_tests.py --filter restart
    python tests/run_tests.py --verbose
    python tests/run_tests.py --inspeccy <path>
    python tests/run_tests.py --update-refs       # rewrite video reference BMPs

Exit code 0 = all pass, 1 = at least one failed, 2 = runner error.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any
sys.path.insert(0, str(Path(__file__).resolve().parent / "python"))
from test_support import test_rom


# ---------------------------------------------------------------------------
# Test-file parsing
# ---------------------------------------------------------------------------

def parse_test_file(path: Path) -> dict[str, Any]:
    """Parse a .test file into a dict. Repeating keys become lists."""
    result: dict[str, Any] = {}
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        eq = line.find("=")
        if eq < 0:
            raise ValueError(f"{path}:{lineno}: no '=' in {line!r}")
        key = line[:eq].strip()
        val = line[eq + 1:].strip()
        if key in result:
            existing = result[key]
            if isinstance(existing, list):
                existing.append(val)
            else:
                result[key] = [existing, val]
        else:
            result[key] = val
    return result


def as_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def parse_int_maybe_hex(s: str) -> int:
    s = s.strip()
    if s.startswith(("0x", "0X")):
        return int(s[2:], 16)
    if s.startswith("$"):
        return int(s[1:], 16)
    return int(s)


# ---------------------------------------------------------------------------
# JSON traversal for status / dump field checks
# ---------------------------------------------------------------------------

def json_path_lookup(root: Any, dotted: str) -> Any:
    node = root
    for part in dotted.split("."):
        if node is None:
            return None
        if part.isdigit():
            try:
                node = node[int(part)]
            except (IndexError, TypeError, KeyError):
                return None
        else:
            if isinstance(node, dict):
                node = node.get(part)
            else:
                node = getattr(node, part, None)
    return node


def json_node_to_string(node: Any) -> str:
    if node is None:
        return "null"
    if isinstance(node, bool):
        return "true" if node else "false"
    if isinstance(node, float):
        # Match PowerShell's $Node display: integers print without .0
        if node.is_integer():
            return str(int(node))
        return str(node)
    return str(node)


def compare_numeric_or_string(actual: str, expected: str) -> bool:
    """If both parse as float, compare within epsilon. Otherwise exact string."""
    try:
        a = float(actual)
        e = float(expected)
        return abs(a - e) < 1e-6
    except ValueError:
        return actual == expected


# ---------------------------------------------------------------------------
# Byte / SHA1 helpers
# ---------------------------------------------------------------------------

def file_sha1(path: Path) -> str:
    h = hashlib.sha1()
    h.update(path.read_bytes())
    return h.hexdigest().lower()


def check_bytes_at(
    file_bytes: bytes,
    pairs: list[str],
    label: str,
    reasons: list[str],
) -> bool:
    """Returns True iff any byte check failed (and appends reasons)."""
    failed = False
    for pair in pairs:
        eq = pair.find("=")
        if eq < 0:
            reasons.append(f"bad {label} {pair!r} (need offset=value)")
            failed = True
            continue
        try:
            off = parse_int_maybe_hex(pair[:eq])
            val = parse_int_maybe_hex(pair[eq + 1:])
        except ValueError:
            reasons.append(f"bad {label} {pair!r}")
            failed = True
            continue
        if off < 0 or off >= len(file_bytes):
            reasons.append(f"offset {off} out of range (file size {len(file_bytes)})")
            failed = True
            continue
        actual = file_bytes[off]
        if actual != val:
            reasons.append(f"{label} byte at {off} (0x{off:X}) = 0x{actual:02X} (expected 0x{val:02X})")
            failed = True
    return failed


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

class Runner:
    def __init__(self, args: argparse.Namespace) -> None:
        self.tests_root = Path(__file__).resolve().parent
        self.project_root = self.tests_root.parent
        self.cases_dir = self.tests_root / "cases"
        self.asm_root = self.tests_root / "asm"
        self.refs_root = self.tests_root / "refs"
        self.tmp_dir = self.tests_root / "tmp"
        self.tmp_dir.mkdir(parents=True, exist_ok=True)

        self.inspeccy = self._locate_inspeccy(args.inspeccy)
        self.filter = args.filter
        self.verbose = args.verbose
        self.update_refs = args.update_refs

        self.pass_count = 0
        self.fail_count = 0
        self.skip_count = 0
        self.xfail_count = 0

    def _locate_inspeccy(self, override: str | None) -> Path:
        if override:
            p = Path(override).resolve()
            if not p.exists():
                print(f"ERROR: inspeccy binary not found at {p}", file=sys.stderr)
                sys.exit(2)   # runner error, per tests/README exit-code legend (not 1 = test failure)
            return p
        # Top-level CMake puts the inspeccy binary under build/inspeccy/.
        for candidate in (
            self.project_root / "build" / "inspeccy" / "Release" / "inspeccy.exe",
            self.project_root / "build" / "inspeccy" / "Release" / "inspeccy",
            self.project_root / "build" / "inspeccy" / "inspeccy.exe",
            self.project_root / "build" / "inspeccy" / "inspeccy",
            self.project_root / "build-top" / "inspeccy" / "inspeccy",
            self.project_root / "build" / "Release" / "inspeccy.exe",
            self.project_root / "build" / "Release" / "inspeccy",
            self.project_root / "build" / "inspeccy",
        ):
            if candidate.exists():
                return candidate
        print("ERROR: inspeccy binary not found in build/. Pass --inspeccy <path>.", file=sys.stderr)
        sys.exit(2)   # runner error, per tests/README exit-code legend (not 1 = test failure)

    def _name_matches(self, name: str) -> bool:
        if not self.filter:
            return True
        # Glob if the filter contains wildcards; substring otherwise.
        if any(c in self.filter for c in "*?[]"):
            return fnmatch.fnmatch(name, self.filter)
        return self.filter in name

    # ----- Case-file tests -------------------------------------------------

    def run_case_file_tests(self) -> None:
        files = sorted(self.cases_dir.glob("*.test"))
        for f in files:
            name = f.stem
            if not self._name_matches(name):
                continue
            try:
                self._run_case_file(name, f)
            except Exception as e:  # noqa: BLE001
                print(f"[FAIL] {name} : runner error: {e}")
                self.fail_count += 1

    def _default_rom(self, name):
        return test_rom(name)

    def _run_case_file(self, name: str, path: Path) -> None:
        try:
            T = parse_test_file(path)
        except Exception as e:  # noqa: BLE001
            print(f"[FAIL] {name} : parse error: {e}")
            self.fail_count += 1
            return

        if "description" not in T:
            print(f"[FAIL] {name} : missing mandatory 'description' field")
            self.fail_count += 1
            return
        desc = T["description"]

        if T.get("skip") == "true":
            print(f"[SKIP] {name} : {desc}")
            self.skip_count += 1
            return

        # exec= mode: self-contained binary
        if "exec" in T:
            self._run_exec_case(name, T)
            return

        if "expect_exit" not in T:
            print(f"[FAIL] {name} : missing mandatory 'expect_exit'")
            self.fail_count += 1
            return
        if "seconds" not in T and "cycles" not in T \
                and T.get("create_initial_dump") != "true":
            print(f"[FAIL] {name} : missing mandatory 'seconds' (or 'cycles')")
            self.fail_count += 1
            return
        if not any(k in T for k in ("sna", "phase1_sna", "dot", "nex", "dck")):
            print(f"[FAIL] {name} : need 'sna', 'phase1_sna', 'dot', 'nex', or 'dck'")
            self.fail_count += 1
            return

        rom48 = (self.project_root / T["rom48"]) if "rom48" in T else self._default_rom(T.get("rom48_name", "128.rom"))
        rom128 = (self.project_root / T["rom128"]) if "rom128" in T else self._default_rom("128.rom")

        # No ROM images ship with the tree (see README, ROMs section). Tests
        # that boot a classic Spectrum need one; skip them when it's absent.
        # nex/dot tests run ROM-free, so only the actual ROM users skip.
        b_needs_rom = any(k in T for k in ("sna", "phase1_sna", "tap", "trd", "dck", "rom48", "rom128"))
        b_have_rom = rom48.is_file() and rom128.is_file()
        if b_needs_rom and not b_have_rom:
            print(f"[SKIP] {name}: ROM image not present (set --rom-dir to your ROM directory)")
            self.skip_count += 1
            return

        # Phase 1: produce a dump for phase 2 to resume from
        phase1_dump_path: Path | None = None
        if "phase1_sna" in T:
            phase1_dump_path = self._run_phase1(name, T, rom48, rom128)
            if phase1_dump_path is None:
                return  # already printed failure

        # Phase 2 args
        out_sna = self.tmp_dir / f"{name}.out.sna"
        cmd: list[str] = [str(self.inspeccy)]
        if phase1_dump_path is not None:
            cmd += ["--resume", str(phase1_dump_path)]
        elif "dot" in T:
            cmd += ["--dot", str(self.tests_root / T["dot"])]
            if "dot_arg" in T:
                cmd += ["--dot-arg", T["dot_arg"]]
            if "vfs" in T:
                cmd += ["--vfs", str(self.tests_root / T["vfs"])]
            if "vfs_drive" in T:
                cmd += ["--vfs-drive", T["vfs_drive"]]
            if T.get("create_initial_dump") == "true":
                cmd += ["--create-initial-dump"]
        elif "nex" in T:
            cmd += ["--nex", str(self.tests_root / T["nex"])]
            if T.get("create_initial_dump") == "true":
                cmd += ["--create-initial-dump"]
        elif "dck" in T:
            # A Timex cartridge is a boot source on its own: the 2068 cold-boots
            # its Home ROM, which starts the LROS out of the dock.
            cmd += ["--dck", str(self.tests_root / T["dck"])]
        else:
            cmd += ["--sna", str(self.tests_root / T["sna"])]

        if "cycles" in T:
            cmd += ["--cycles", T["cycles"]]     # exact-cycle budget (turbo-invariant pins)
        elif T.get("create_initial_dump") != "true" or "seconds" in T:
            cmd += ["--seconds", T["seconds"]]
        if b_have_rom:
            cmd += ["--rom48", str(rom48), "--rom128", str(rom128)]

        if "rom_trdos" in T or "trd" in T:
            rom_trd = (self.project_root / T["rom_trdos"]) if "rom_trdos" in T else self._default_rom("trdos504t.rom")
            if not rom_trd.is_file():
                print(f"[SKIP] {name}: TR-DOS ROM image not present (supply trdos504t.rom through --rom-dir)")
                self.skip_count += 1
                return
            cmd += ["--rom-trdos", str(rom_trd)]
        if "trd" in T:
            cmd += ["--trd", str(self.tests_root / T["trd"])]
        if "machine" in T:
            cmd += ["--machine", T["machine"]]
        # A writable scratch mount for esxDOS write/dir tests: a fresh dir under
        # tmp/ (auto-wiped), passed as --vfs for ANY source (nex/dot). The guest
        # program creates its own files there, so nothing needs seeding and the
        # committed tree stays clean.
        if "vfs_scratch" in T:
            scratch = self.tmp_dir / T["vfs_scratch"]
            if scratch.exists():
                shutil.rmtree(scratch, ignore_errors=True)
            scratch.mkdir(parents=True, exist_ok=True)
            cmd += ["--vfs", str(scratch)]
        if "mhz" in T:
            cmd += ["--mhz", T["mhz"]]
        if "input" in T:
            cmd += ["--input", str(self.tests_root / T["input"])]
        if "input_tap_frames" in T:
            cmd += ["--input-tap-frames", T["input_tap_frames"]]
        if T.get("reset_trap") == "off":
            cmd += ["--reset-trap", "off"]
        if T.get("no_interrupts") == "true":
            cmd += ["--no-interrupts"]

        needs_trace = "expect_trace_contains" in T or "expect_trace_matches" in T
        trace_path = self.tmp_dir / f"{name}.trace"
        if needs_trace:
            cmd += ["--trace-out", str(trace_path)]

        cmd += ["--out", str(out_sna)]

        needs_psg = (
            "expect_psg_sha1" in T or "expect_psg_byte_at" in T or T.get("psg_out") == "auto"
        )
        psg_path = self.tmp_dir / f"{name}.psg"
        if needs_psg:
            cmd += ["--psg-out", str(psg_path)]
        if "psg_rate" in T:
            cmd += ["--psg-rate", T["psg_rate"]]
        if T.get("no_ay") == "true":
            cmd += ["--no-ay"]

        if "audio_out" in T:
            cmd += ["--audio-out", str(self.tmp_dir / T["audio_out"])]

        for extra in as_list(T.get("extra_args")):
            cmd += extra.split()

        # Wipe prior artifacts.
        base = str(out_sna)[:-4] if str(out_sna).endswith(".sna") else str(out_sna)
        pre_paths = [
            trace_path, out_sna, psg_path,
            Path(base + ".bmp"),
            Path(base + ".status.json"),
            Path(base + ".inspec.dump.json"),
            self.tmp_dir / f"{name}.stdout",
            self.tmp_dir / f"{name}.stderr",
        ]
        for p in pre_paths:
            p.unlink(missing_ok=True)

        stdout_path = self.tmp_dir / f"{name}.stdout"
        stderr_path = self.tmp_dir / f"{name}.stderr"
        # Wall-clock cap so a run that never terminates (e.g. an emulator
        # livelock that no --cycles budget can bound) FAILS the test instead of
        # hanging the whole suite. Default 120s covers the slowest real case;
        # a `timeout_s = N` directive tightens it for tests that assert
        # termination itself.
        timeout_s = float(T["timeout_s"]) if "timeout_s" in T else 120.0
        with open(stdout_path, "w", encoding="utf-8") as so, open(stderr_path, "w", encoding="utf-8") as se:
            try:
                result = subprocess.run(cmd, stdout=so, stderr=se, timeout=timeout_s)
                exit_code = result.returncode
            except subprocess.TimeoutExpired:
                print(f"[FAIL] {name} : did not terminate within {timeout_s:g}s "
                      f"(possible livelock/hang)")
                if self.verbose:
                    print(f"        cmd: {' '.join(cmd)}")
                self.fail_count += 1
                return

        stdout_lines = stdout_path.read_text(encoding="utf-8", errors="replace").splitlines() if stdout_path.exists() else []
        summary_line = stdout_lines[-1] if stdout_lines else ""

        failed = False
        reasons: list[str] = []

        expected_exit = int(T["expect_exit"])
        if exit_code != expected_exit:
            failed = True
            reasons.append(f"exit={exit_code} (expected {expected_exit})")

        for p in as_list(T.get("expect_summary_contains")):
            if p not in summary_line:
                failed = True
                reasons.append(f"summary missing {p!r}")
        for p in as_list(T.get("expect_summary_matches")):
            if not re.search(p, summary_line):
                failed = True
                reasons.append(f"summary !~ /{p}/")

        if needs_trace:
            if not trace_path.exists():
                failed = True
                reasons.append(f"trace file not produced at {trace_path}")
            else:
                trace_text = trace_path.read_text(encoding="utf-8", errors="replace")
                for p in as_list(T.get("expect_trace_contains")):
                    if p not in trace_text:
                        failed = True
                        reasons.append(f"trace missing {p!r}")
                for p in as_list(T.get("expect_trace_matches")):
                    if not re.search(p, trace_text):
                        failed = True
                        reasons.append(f"trace !~ /{p}/")

        if "expect_output_sna_sha1" in T:
            if not out_sna.exists():
                failed = True
                reasons.append(f"output sna not produced at {out_sna}")
            else:
                actual = file_sha1(out_sna)
                expected = T["expect_output_sna_sha1"].lower()
                if actual != expected:
                    failed = True
                    reasons.append(f"sha1={actual} (expected {expected})")

        if "expect_output_sna_byte_at" in T:
            if not out_sna.exists():
                failed = True
                reasons.append(f"output sna not produced at {out_sna}")
            else:
                if check_bytes_at(out_sna.read_bytes(),
                                  as_list(T["expect_output_sna_byte_at"]),
                                  "expect_output_sna_byte_at",
                                  reasons):
                    failed = True

        if "expect_psg_byte_at" in T:
            if not psg_path.exists():
                failed = True
                reasons.append(f"psg file not produced at {psg_path}")
            else:
                if check_bytes_at(psg_path.read_bytes(),
                                  as_list(T["expect_psg_byte_at"]),
                                  "expect_psg_byte_at",
                                  reasons):
                    failed = True

        bmp_path = Path(base + ".bmp")
        if "expect_bmp_byte_at" in T:
            if not bmp_path.exists():
                failed = True
                reasons.append(f"bmp file not produced at {bmp_path}")
            else:
                if check_bytes_at(bmp_path.read_bytes(),
                                  as_list(T["expect_bmp_byte_at"]),
                                  "expect_bmp_byte_at",
                                  reasons):
                    failed = True

        if "expect_bmp_sha1" in T:
            if not bmp_path.exists():
                failed = True
                reasons.append(f"bmp file not produced at {bmp_path}")
            else:
                actual = file_sha1(bmp_path)
                expected = T["expect_bmp_sha1"].lower()
                if actual != expected:
                    failed = True
                    reasons.append(f"bmp sha1={actual} (expected {expected})")

        if "expect_bmp_equals" in T:
            ref_path = self.tests_root / T["expect_bmp_equals"]
            if not bmp_path.exists():
                failed = True
                reasons.append(f"bmp file not produced at {bmp_path}")
            elif not ref_path.exists():
                failed = True
                reasons.append(f"reference bmp missing: {ref_path}")
            else:
                ref_bytes = ref_path.read_bytes()
                ac_bytes = bmp_path.read_bytes()
                if len(ref_bytes) != len(ac_bytes):
                    failed = True
                    reasons.append(f"bmp size {len(ac_bytes)} != ref size {len(ref_bytes)}")
                else:
                    diffs = []
                    diff_count = 0
                    for i in range(len(ref_bytes)):
                        if ref_bytes[i] != ac_bytes[i]:
                            diff_count += 1
                            if len(diffs) < 5:
                                diffs.append(f"@{i}: ref=0x{ref_bytes[i]:02X} actual=0x{ac_bytes[i]:02X}")
                    if diff_count > 0:
                        failed = True
                        reasons.append(f"bmp differs from ref ({diff_count} bytes)")
                        for d in diffs:
                            reasons.append("  " + d)

        status_path = Path(base + ".status.json")
        if "expect_status_field_eq" in T:
            failed |= self._check_json_field_eq(
                status_path, "status", as_list(T["expect_status_field_eq"]), reasons,
            )

        dump_path = Path(base + ".inspec.dump.json")
        if "expect_dump_field_eq" in T:
            failed |= self._check_json_field_eq(
                dump_path, "dump", as_list(T["expect_dump_field_eq"]), reasons,
            )
        if "expect_dump_field_ne" in T:
            failed |= self._check_json_field_eq(
                dump_path, "dump", as_list(T["expect_dump_field_ne"]), reasons,
                b_negate=True,
            )

        if "expect_file_absent" in T:
            for entry in as_list(T["expect_file_absent"]):
                key = entry.strip()
                target = {
                    "bmp": bmp_path,
                    "status": status_path,
                    "psg": psg_path,
                    "dump": dump_path,
                }.get(key, self.tmp_dir / key)
                if target.exists():
                    failed = True
                    reasons.append(f"file should be absent but exists: {target}")

        if "expect_psg_sha1" in T:
            if not psg_path.exists():
                failed = True
                reasons.append(f"psg file not produced at {psg_path}")
            else:
                actual = file_sha1(psg_path)
                expected = T["expect_psg_sha1"].lower()
                if actual != expected:
                    failed = True
                    reasons.append(f"psg sha1={actual} (expected {expected})")

        xfail = T.get("xfail") == "true"
        if xfail:
            if failed:
                print(f"[XFAIL] {name} : {desc}")
                self.xfail_count += 1
            else:
                print(f"[XPASS] {name} : {desc} (expected failure but passed)")
                self.fail_count += 1
        else:
            if failed:
                print(f"[FAIL] {name} : {desc}")
                for r in reasons:
                    print(f"        {r}")
                if self.verbose:
                    print("        ---- summary line ----")
                    print(f"        {summary_line}")
                    if stderr_path.exists():
                        tail = stderr_path.read_text(encoding="utf-8", errors="replace").splitlines()[-10:]
                        if tail:
                            print("        ---- stderr (last 10) ----")
                            for line in tail:
                                print(f"        {line}")
                else:
                    print(f"        (artifacts: {self.tmp_dir}/{name}.{{stdout,stderr,trace}})")
                self.fail_count += 1
            else:
                print(f"[PASS] {name}")
                self.pass_count += 1

    def _check_json_field_eq(
        self,
        json_path: Path,
        label: str,
        pairs: list[str],
        reasons: list[str],
        b_negate: bool = False,
    ) -> bool:
        failed = False
        if not json_path.exists():
            reasons.append(f"{label} file not produced at {json_path}")
            return True
        try:
            obj = json.loads(json_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            reasons.append(f"{label} file is not valid JSON: {e}")
            return True
        for pair in pairs:
            eq = pair.find("=")
            if eq < 0:
                reasons.append(f"bad expect_{label}_field_eq {pair!r} (need path=value)")
                failed = True
                continue
            dotted = pair[:eq].strip()
            expected = pair[eq + 1:].strip()
            node = json_path_lookup(obj, dotted)
            actual = json_node_to_string(node)
            matched = compare_numeric_or_string(actual, expected)
            if matched == b_negate:
                failed = True
                want = f"anything but {expected}" if b_negate else expected
                reasons.append(f"{label}.{dotted} = {actual} (expected {want})")
        return failed

    def _run_phase1(
        self, name: str, T: dict[str, Any], rom48: Path, rom128: Path
    ) -> Path | None:
        out_sna = self.tmp_dir / f"{name}.phase1.out.sna"
        dump_path = self.tmp_dir / f"{name}.phase1.out.inspec.dump.json"
        for p in (out_sna, dump_path):
            p.unlink(missing_ok=True)
        cmd = [
            str(self.inspeccy),
            "--sna", str(self.tests_root / T["phase1_sna"]),
            "--seconds", T["phase1_seconds"],
            "--rom48", str(rom48), "--rom128", str(rom128),
            "--out", str(out_sna),
            "--no-bmp", "--no-status",
        ]
        # `no_interrupts` propagates from the parent test directive to phase 1
        # too — cycle-accounting tests need it on BOTH passes for the dumped
        # cycle counts to match across the resume boundary.
        if T.get("no_interrupts") == "true":
            cmd += ["--no-interrupts"]
        if "phase1_extra_args" in T:
            for extra in as_list(T["phase1_extra_args"]):
                cmd += extra.split()
        stdout_path = self.tmp_dir / f"{name}.phase1.stdout"
        stderr_path = self.tmp_dir / f"{name}.phase1.stderr"
        with open(stdout_path, "w") as so, open(stderr_path, "w") as se:
            subprocess.run(cmd, stdout=so, stderr=se)
        if not dump_path.exists():
            print(f"[FAIL] {name} : phase 1 did not produce dump file")
            print(f"        (artifacts: {self.tmp_dir}/{name}.phase1.*)")
            self.fail_count += 1
            return None
        return dump_path

    def _run_exec_case(self, name: str, T: dict[str, Any]) -> None:
        exec_path = Path(T["exec"])
        if not exec_path.is_absolute():
            exec_path = self.tests_root / exec_path
        if not exec_path.exists():
            print(f"[FAIL] {name} : exec binary not found at {exec_path}")
            self.fail_count += 1
            return
        stdout_path = self.tmp_dir / f"{name}.stdout"
        stderr_path = self.tmp_dir / f"{name}.stderr"
        with open(stdout_path, "w") as so, open(stderr_path, "w") as se:
            result = subprocess.run([str(exec_path)], stdout=so, stderr=se)
        expected_exit = int(T["expect_exit"])
        if result.returncode == expected_exit:
            print(f"[PASS] {name}")
            self.pass_count += 1
        else:
            print(f"[FAIL] {name} : exit={result.returncode} (expected {expected_exit})")
            if self.verbose and stderr_path.exists():
                for line in stderr_path.read_text(encoding="utf-8", errors="replace").splitlines():
                    print(f"        {line}")
            self.fail_count += 1

    # ----- Assembly tests --------------------------------------------------

    def run_asm_tests(self) -> None:
        if not self.asm_root.exists():
            return
        sjasmplus = shutil.which("sjasmplus")
        if not sjasmplus:
            print("[FAIL] asm-tests : sjasmplus not on PATH; skipping all .asm tests")
            self.fail_count += 1
            return

        asm_files = sorted(self.asm_root.rglob("*.asm"))
        # Skip files whose basename starts with `_` (= include-only fragments).
        asm_files = [f for f in asm_files if not f.stem.startswith("_")]
        if self.filter:
            if any(c in self.filter for c in "*?[]"):
                asm_files = [f for f in asm_files if fnmatch.fnmatch(f.stem, self.filter)
                             or fnmatch.fnmatch(str(f), self.filter)]
            else:
                asm_files = [f for f in asm_files if self.filter in f.stem
                             or self.filter in str(f).replace("\\", "/")]

        for asm in asm_files:
            self._run_asm_test(asm, sjasmplus)

    def _run_asm_test(self, asm: Path, sjasmplus: str) -> None:
        rel = asm.relative_to(self.asm_root).as_posix()
        test_name = "asm/" + rel[:-len(".asm")]
        dot_path = self.tmp_dir / "asm" / (rel[:-len(".asm")] + ".dot")
        dot_path.parent.mkdir(parents=True, exist_ok=True)

        asm_stdout = self.tmp_dir / "asm" / (rel[:-len(".asm")] + ".asm.stdout")
        asm_stderr = self.tmp_dir / "asm" / (rel[:-len(".asm")] + ".asm.stderr")
        with open(asm_stdout, "w") as so, open(asm_stderr, "w") as se:
            asm_proc = subprocess.run(
                [sjasmplus, f"-i{self.asm_root}", f"--raw={dot_path}", str(asm)],
                stdout=so, stderr=se,
            )
        diagnostics = asm_stderr.read_text(encoding="utf-8", errors="replace")
        if asm_proc.returncode != 0 or re.search(r"\bwarning:", diagnostics, re.IGNORECASE):
            print(f"[FAIL] {test_name} : sjasmplus returned {asm_proc.returncode} or emitted a warning")
            print(diagnostics.rstrip())
            if self.verbose:
                for line in asm_stdout.read_text(encoding="utf-8", errors="replace").splitlines():
                    print(f"        {line}")
            self.fail_count += 1
            return

        run_stdout = self.tmp_dir / "asm" / (rel[:-len(".asm")] + ".stdout")
        run_stderr = self.tmp_dir / "asm" / (rel[:-len(".asm")] + ".stderr")
        bmp_path = self.tmp_dir / "asm" / (rel[:-len(".asm")] + ".bmp")
        bmp_path.unlink(missing_ok=True)
        wav_path = self.tmp_dir / "asm" / (rel[:-len(".asm")] + ".wav")
        wav_path.unlink(missing_ok=True)

        is_video = rel.startswith("video/")
        # An asm test that contains a `; @needs-audio-out` line in its source
        # opts into the audio-test path: the runner adds --audio-out and after
        # the run checks the WAV was actually produced (= non-empty), which is
        # how we regression-test the DAC ports without depending on a real
        # demo binary. Cheap to parse — one Path.read_text + `in`.
        asm_text = asm.read_text(encoding="utf-8", errors="replace")
        needs_audio = "@needs-audio-out" in asm_text
        # `; @ram-mb N` in the source pins this test to a specific physical
        # RAM size (otherwise the runner's --ram-mb default applies). Tests
        # that exercise open-bus / 1 MB allocator semantics opt into this.
        ram_mb_override: int | None = None
        for line in asm_text.splitlines():
            m = re.match(r"\s*;\s*@ram-mb\s+(\d+)", line)
            if m:
                ram_mb_override = int(m.group(1))
                break
        # `; @uart-pi loop` attaches a loopback to the Next's Pi UART so the
        # $133B..$163B register interface can be exercised without a process.
        uart_pi: str | None = None
        for line in asm_text.splitlines():
            m = re.match(r"\s*;\s*@uart-pi\s+(\S+)", line)
            if m:
                uart_pi = m.group(1)
                break

        cmd = [
            str(self.inspeccy),
            "--dot", str(dot_path),
            "--machine", "next50",
            "--seconds", "1",
            "--no-status", "--no-dump",
        ]
        # ROMs are optional for --dot runs; pass them only when present.
        asm_rom = self._default_rom("128.rom")
        if asm_rom.is_file():
            cmd[3:3] = ["--rom48", str(asm_rom), "--rom128", str(asm_rom)]
        if is_video:
            # These goldens assert the COMPOSITOR (ComposePixel) pixel math on a
            # single end-of-run frame, so they pin the snapshot renderer with
            # --bmp-exact off. The frame-exact default (checkpoint+replay through
            # the eager beam drawer) is exercised by test_bmp_frame_exact.py; it
            # deliberately differs on programs that rewrite the whole framebuffer
            # across the visible frame every frame, which several of these
            # synthetic fills do.
            cmd += ["--bmp-out", str(bmp_path), "--bmp-exact", "off"]
        else:
            cmd += ["--no-bmp"]
        if uart_pi:
            cmd += ["--uart-pi", uart_pi]
        if needs_audio:
            cmd += ["--audio-out", str(wav_path)]
        if ram_mb_override is not None:
            cmd += ["--ram-mb", str(ram_mb_override)]

        with open(run_stdout, "w") as so, open(run_stderr, "w") as se:
            run_proc = subprocess.run(cmd, stdout=so, stderr=se)

        failed = False
        reasons: list[str] = []
        if run_proc.returncode != 5:
            failed = True
            reasons.append(
                f"exit={run_proc.returncode} (expected 5/RestartTrap; 2=HaltedInterruptsDisabled = a CHECK_* macro fired)"
            )

        # DAC regression check: when --audio-out was requested, the
        # WAV must exist AND contain at least 1 byte of PCM (= the test
        # successfully exercised port $DF or $1F capture).
        if needs_audio and not failed:
            if not wav_path.exists():
                failed = True
                reasons.append(f"no audio WAV produced at {wav_path}")
            elif wav_path.stat().st_size <= 44:    # 44 = WAV header size; need >= 1 sample
                failed = True
                reasons.append(
                    f"audio WAV at {wav_path} is empty "
                    f"(size={wav_path.stat().st_size}; header is 44 bytes)")

        if is_video and not failed:
            ref_path = self.refs_root / (rel[:-len(".asm")] + ".bmp")
            if self.update_refs:
                ref_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(bmp_path, ref_path)
                print(f"[REFR] {test_name} : updated reference at {ref_path}")
            elif not ref_path.exists():
                failed = True
                reasons.append(f"no reference at {ref_path} (run with --update-refs to create it)")
            else:
                if file_sha1(bmp_path) != file_sha1(ref_path):
                    failed = True
                    reasons.append(f"bmp differs from {ref_path} (actual={bmp_path})")

        if failed:
            print(f"[FAIL] {test_name}")
            for r in reasons:
                print(f"        {r}")
            if self.verbose:
                lines = run_stdout.read_text(encoding="utf-8", errors="replace").splitlines()
                if lines:
                    print(f"        {lines[-1]}")
            self.fail_count += 1
        else:
            print(f"[PASS] {test_name}")
            self.pass_count += 1


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--filter", default="", help="Substring or glob; matches case names + asm paths.")
    p.add_argument("--inspeccy", dest="inspeccy", default=None,
                   help="Override inspeccy executable path.")
    p.add_argument("--verbose", action="store_true", help="Dump stdout summary + last 10 stderr lines on FAIL.")
    p.add_argument("--update-refs", action="store_true",
                   help="Overwrite reference BMPs for asm/video/* tests from the current run.")
    p.add_argument("--rom-dir", help="Directory containing optional ROM images supplied by the user.")
    p.add_argument("--require-no-skips", action="store_true", help="Fail if any selected test is skipped.")
    args = p.parse_args(argv)

    runner = Runner(args)
    os.environ["INSPECCY"] = str(runner.inspeccy)
    if args.rom_dir:
        os.environ["INSPECCY_ROM_DIR"] = str(Path(args.rom_dir).resolve())
    runner.run_case_file_tests()
    runner.run_asm_tests()

    # Standalone scripted tests — invoked as subprocesses, count toward pass/fail.
    standalone = [
        ("cli_validation", Path(__file__).parent / "python" / "test_cli_validation.py"),
        ("uart_process", Path(__file__).parent / "python" / "test_uart_process.py"),
        ("comparison_tools", Path(__file__).parent / "python" / "test_comparison_tools.py"),
        ("btr_roundtrip", Path(__file__).parent / "python" / "test_btr_roundtrip.py"),
        ("btr_fidx",      Path(__file__).parent / "python" / "test_btr_fidx.py"),
        ("copper_render", Path(__file__).parent / "python" / "test_copper_render.py"),
        ("coverage_128k_paging", Path(__file__).parent / "python" / "test_coverage_128k_paging.py"),
        ("bmp_classic_ula", Path(__file__).parent / "python" / "test_bmp_classic_ula.py"),
        ("bmp_timex_scld", Path(__file__).parent / "python" / "test_bmp_timex_scld.py"),
        ("z80_load", Path(__file__).parent / "python" / "test_z80_load.py"),
        ("tap_load", Path(__file__).parent / "python" / "test_tap_load.py"),
        ("layer2_window", Path(__file__).parent / "python" / "test_layer2_window.py"),
        ("bmp_frame_exact", Path(__file__).parent / "python" / "test_bmp_frame_exact.py"),
        ("floating_bus", Path(__file__).parent / "python" / "test_floating_bus.py"),
        ("copper_waitless", Path(__file__).parent / "python" / "test_copper_waitless.py"),
        ("copper_scroll_video", Path(__file__).parent / "python" / "test_copper_scroll_video.py"),
        ("resume_equivalence", Path(__file__).parent / "python" / "test_resume_equivalence.py"),
        ("plus3_paging", Path(__file__).parent / "python" / "test_plus3_paging.py"),
        ("next_mmu_rom_raster", Path(__file__).parent / "python" / "test_next_mmu_rom_raster.py"),
        ("trdos_run", Path(__file__).parent / "python" / "test_trdos_run.py"),
        ("zxevo_baseconf", Path(__file__).parent / "python" / "test_zxevo_baseconf.py"),
        ("zxuno", Path(__file__).parent / "python" / "test_zxuno.py"),
        ("zx_clones_resume", Path(__file__).parent / "python" / "test_zx_clones_resume.py"),
        ("zxevo_atm_modes", Path(__file__).parent / "python" / "test_zxevo_atm_modes.py"),
        ("zxuno_radastan", Path(__file__).parent / "python" / "test_zxuno_radastan.py"),
        ("contention", Path(__file__).parent / "python" / "test_contention.py"),
        ("audio_render", Path(__file__).parent / "python" / "test_audio_render.py"),
        ("peephole",     Path(__file__).parent / "python" / "test_peephole.py"),
        ("z80n_gate",    Path(__file__).parent / "python" / "test_z80n_gate.py"),
        ("z80_fingerprint", Path(__file__).parent / "python" / "test_z80_fingerprint.py"),
        ("cycle_exact_video", Path(__file__).parent / "python" / "test_cycle_exact_video.py"),
        ("z80_tables_fresh", Path(__file__).parent / "python" / "test_z80_tables_fresh.py"),
        ("z80_effects_fresh", Path(__file__).parent / "python" / "test_z80_effects_fresh.py"),
        ("until_pc", Path(__file__).parent / "python" / "test_until_pc.py"),
        ("plain_dispatch_equiv", Path(__file__).parent / "python" / "test_plain_dispatch_equiv.py"),
        ("resume_input", Path(__file__).parent / "python" / "test_resume_input.py"),
        ("int_window", Path(__file__).parent / "python" / "test_int_window.py"),
        ("line_int", Path(__file__).parent / "python" / "test_line_int.py"),
    ]
    for name, path in standalone:
        if args.filter and not (args.filter in name or args.filter in str(path)):
            continue
        if not path.is_file():
            print(f"[SKIP] {name}: {path} missing")
            runner.skip_count += 1
            continue
        rc = subprocess.run(
            [sys.executable, str(path)] + ([str(runner.inspeccy)] if name == "uart_process" else []),
            cwd=str(Path(__file__).parent.parent)).returncode
        if rc == 0:
            print(f"[PASS] {name}")
            runner.pass_count += 1
        elif rc == 77:
            # 77 = "prerequisite unavailable" (e.g. a ROM image, which does
            # not ship with the tree) — skip, don't fail.
            print(f"[SKIP] {name}: prerequisite unavailable")
            runner.skip_count += 1
        else:
            print(f"[FAIL] {name}: exit {rc}")
            runner.fail_count += 1

    total = runner.pass_count + runner.fail_count + runner.skip_count + runner.xfail_count
    print()
    print(f"Results: {runner.pass_count} passed, {runner.fail_count} failed, "
          f"{runner.skip_count} skipped, {runner.xfail_count} xfailed ({total} total)")
    return 1 if runner.fail_count or (args.require_no_skips and runner.skip_count) else 0


if __name__ == "__main__":
    sys.exit(main())
