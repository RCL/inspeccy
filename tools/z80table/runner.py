"""Driver for the inspeccy CLI (batch emulator), for the table tools beside it.

inspeccy is argv-only: one process per run, state in via a snapshot, state out
via --trace-out / --dump-out / --status-out. This module wraps that contract
and parses the three artifacts.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional



class ToolError(RuntimeError):
    """The emulator could not be found, run, or its output read."""

# Exit codes pinned in inspeccy/src/main.cpp.
EXIT_BUDGET = 0
EXIT_INVALID_OPCODE = 1
EXIT_HALT_DI = 2            # halted with interrupts disabled
EXIT_IO = 3
EXIT_ARGS = 4
EXIT_RESTART_TRAP = 5       # PC reached $0000
EXIT_DELIBERATE_STOP = 6    # strict DI; HALT
EXIT_REACHED_PC = 8         # --until-pc target reached
EXIT_NAMES = {
    0: "BudgetReached", 1: "InvalidOpcode", 2: "HaltedInterruptsDisabled", 3: "IoError",
    4: "BadArgs", 5: "RestartTrap", 6: "HaltedDeliberateStop", 7: "InvariantViolation",
    8: "ReachedPc",
}


def _candidates() -> list[Path]:
    root = Path(__file__).resolve().parents[2]
    return [root / folder / name
            for folder in ("build/inspeccy/Release", "build/inspeccy", "bin")
            for name in ("inspeccy.exe", "inspeccy")]


def locate_inspeccy(override: str | os.PathLike | None = None) -> Path:
    """Find the inspeccy binary: explicit arg, $INSPECCY, then the
    workspace's build/install locations, then PATH."""
    if override:
        p = Path(override)
        if p.is_file():
            return p.resolve()
        raise ToolError(f"inspeccy not found at {p}")
    env = os.environ.get("INSPECCY")
    if env:
        p = Path(env)
        if p.is_file():
            return p.resolve()
        raise ToolError(f"$INSPECCY={env} does not exist")
    for c in _candidates():
        if c.is_file():
            return c.resolve()
    import shutil
    w = shutil.which("inspeccy")
    if w:
        return Path(w).resolve()
    raise ToolError(
        "inspeccy binary not found. Build it (build/) or set INSPECCY.")


def locate_rom48(override: str | os.PathLike | None = None) -> Optional[Path]:
    if override:
        p = Path(override)
        return p if p.is_file() else None
    env = os.environ.get("INSPECCY_ROM48")
    if env and Path(env).is_file():
        return Path(env)
    directory = os.environ.get("INSPECCY_ROM_DIR")
    if directory:
        candidate = Path(directory) / "48.rom"
        if candidate.is_file():
            return candidate
    return None


@dataclass
class TraceLine:
    cycles: int
    frame: int
    addr: int
    page: str
    mnemonic: str
    effects: str


@dataclass
class RunResult:
    returncode: int
    summary: dict = field(default_factory=dict)   # parsed "result=... cycles=..." line
    stdout: str = ""
    stderr: str = ""
    trace_path: Optional[Path] = None
    dump_path: Optional[Path] = None
    status_path: Optional[Path] = None

    @property
    def result_name(self) -> str:
        return self.summary.get("result") or EXIT_NAMES.get(self.returncode, str(self.returncode))


_SUMMARY_RE = re.compile(r"(\w+)=(\$?[0-9A-Fa-fx.\-]+|\S+)")
_TRACE_RE = re.compile(
    r"^\s*(\d+)\s*\|\s*(\d+)\s*\|\s*\$([0-9A-Fa-f]{4})\s*\(\s*([^)]*)\)\s*\|\s*(.*?)\s*(?:\|\s*(.*))?$")


class Inspeccy:
    def __init__(self, exe: str | os.PathLike | None = None, rom48: str | os.PathLike | None = None):
        self.exe = locate_inspeccy(exe)
        self.rom48 = locate_rom48(rom48)

    def run(self, sna: Path, *, workdir: Path, machine: str = "next50",
            cycles: int | None = None, seconds: float | None = None,
            no_interrupts: bool = True, trace: bool = False, trace_format: str = "minimal",
            dump: bool = False, status: bool = False, contention: bool = False,
            mhz: str | None = None, extra: tuple[str, ...] = (), timeout: float = 120.0,
            stem: str | None = None, until_pc: int | None = None) -> RunResult:
        # absolute paths: the process runs with cwd=workdir, and the caller's
        # paths may be relative to somewhere else
        sna = Path(sna).resolve()
        workdir = Path(workdir).resolve()
        workdir.mkdir(parents=True, exist_ok=True)
        stem = stem or sna.stem
        cmd = [str(self.exe), "--sna", str(sna), "--machine", machine]
        if self.rom48 is not None:
            cmd += ["--rom48", str(self.rom48)]
        if cycles is not None:
            cmd += ["--cycles", str(int(cycles))]
        elif seconds is not None:
            cmd += ["--seconds", str(seconds)]
        else:
            raise ToolError("run(): cycles or seconds required")
        if no_interrupts:
            cmd += ["--no-interrupts"]
        if mhz:
            cmd += ["--mhz", str(mhz)]
        if contention:
            cmd += ["--contention", "on"]
        if until_pc is not None:
            cmd += ["--until-pc", f"0x{until_pc & 0xFFFF:04X}"]
        out_sna = workdir / f"{stem}.out.sna"
        cmd += ["--out", str(out_sna), "--no-bmp"]
        r = RunResult(returncode=-1)
        if trace:
            r.trace_path = workdir / f"{stem}.trace.txt"
            cmd += ["--trace-out", str(r.trace_path), "--trace-format", trace_format]
        if dump:
            r.dump_path = workdir / f"{stem}.dump.json"
            cmd += ["--dump-out", str(r.dump_path)]
        else:
            cmd += ["--no-dump"]
        if status:
            r.status_path = workdir / f"{stem}.status.json"
            cmd += ["--status-out", str(r.status_path)]
        else:
            cmd += ["--no-status"]
        cmd += list(extra)
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=str(workdir))
        except subprocess.TimeoutExpired as e:
            raise ToolError(f"inspeccy timed out after {timeout}s: {' '.join(cmd)}") from e
        r.returncode = p.returncode
        r.stdout, r.stderr = p.stdout, p.stderr
        for line in p.stdout.splitlines():
            if line.startswith("result="):
                r.summary = {k: v for k, v in _SUMMARY_RE.findall(line)}
        if p.returncode in (EXIT_IO, EXIT_ARGS):
            raise ToolError(f"inspeccy failed (exit {p.returncode}): {p.stderr.strip()}\n  cmd: {' '.join(cmd)}")
        return r

    # ----- artifact parsers ---------------------------------------------------

    @staticmethod
    def parse_trace(path: Path) -> list[TraceLine]:
        out: list[TraceLine] = []
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                m = _TRACE_RE.match(line.rstrip("\n"))
                if not m:
                    continue  # interrupt / invalid-opcode banner lines
                cyc, frame, addr, page, mnem, eff = m.groups()
                out.append(TraceLine(int(cyc), int(frame), int(addr, 16), page.strip(),
                                     mnem.strip(), (eff or "").strip()))
        return out

    @staticmethod
    def parse_status(path: Path) -> dict:
        with open(path, encoding="utf-8") as f:
            return json.load(f)

    @staticmethod
    def parse_dump(path: Path) -> "Dump":
        with open(path, encoding="utf-8") as f:
            j = json.load(f)
        return Dump(j)


def _hexval(s) -> int:
    if isinstance(s, int):
        return s
    s = str(s).strip()
    if s.startswith("$"):
        return int(s[1:], 16)
    return int(s, 0)


class Dump:
    """Parsed .inspec.dump.json: registers and RAM banks."""

    def __init__(self, j: dict):
        self.raw = j
        cpu = j.get("cpu", {})
        self.regs: dict[str, int] = {}
        for k in ("AF", "BC", "DE", "HL", "AFp", "BCp", "DEp", "HLp", "IX", "IY", "SP", "PC", "MemPtr"):
            if k in cpu:
                self.regs[k] = _hexval(cpu[k])
        for k in ("I", "R", "IFF1", "IFF2", "IM"):
            if k in cpu:
                self.regs[k] = int(cpu[k])
        self.halted = bool(cpu.get("halted", False))
        self.cycles_total = int(j.get("stats", {}).get("cycles_total", 0))
        self.halt_cycles = int(j.get("stats", {}).get("halt_cycles_total", 0))
        self.port_7ffd = _hexval(j.get("memory", {}).get("port_7ffd", 0))
        self._banks: dict[int, bytes] = {}
        for k, v in j.get("ram", {}).items():
            if not k.startswith("bank_"):
                continue
            self._banks[int(k[5:])] = _decode_bank(v)

    def bank(self, n: int) -> bytes:
        b = self._banks.get(n)
        if b is None:
            return bytes(16384)
        return b

    def mem(self, addr: int, n: int = 1, bank_c000: int | None = None) -> bytes:
        """Read n bytes at a CPU address using 128K-style paging (bank 5 at
        $4000, 2 at $8000, port_7ffd&7 at $C000). ROM ($0000-$3FFF) is not in
        the dump and reads as zeros."""
        out = bytearray()
        a = addr
        while len(out) < n:
            slot = a >> 14
            if slot == 0:
                out.append(0)
                a += 1
                continue
            bnk = {1: 5, 2: 2, 3: (self.port_7ffd & 7) if bank_c000 is None else bank_c000}[slot]
            off = a & 0x3FFF
            take = min(n - len(out), 0x4000 - off)
            out += self.bank(bnk)[off:off + take]
            a += take
        return bytes(out)

    # 8-bit views
    def reg8(self, name: str) -> int:
        pair = {"A": ("AF", 8), "F": ("AF", 0), "B": ("BC", 8), "C": ("BC", 0), "D": ("DE", 8),
                "E": ("DE", 0), "H": ("HL", 8), "L": ("HL", 0), "IXH": ("IX", 8), "IXL": ("IX", 0),
                "IYH": ("IY", 8), "IYL": ("IY", 0)}
        p, sh = pair[name.upper()]
        return (self.regs[p] >> sh) & 0xFF


_LINE_RE = re.compile(r"^\s*\d+:([0-9A-Fa-f]+)\s+(.*)$")


def _decode_bank(v) -> bytes:
    if isinstance(v, str) and v.endswith(":zeros"):
        return bytes(16384)
    buf = bytearray(16384)
    for line in str(v).split("\n"):
        m = _LINE_RE.match(line)
        if not m:
            continue
        off = int(m.group(1), 16)
        data = bytes.fromhex(m.group(2).replace(" ", ""))
        buf[off:off + len(data)] = data
    return bytes(buf)
