"""Empirically check the hand-entered flag / register metadata (meta.py)
against inspeccy. Reads tests/refs/z80_fingerprint/next50.json.

For every canonical row, run the instruction K times from random register
states and compare before/after:

  - a register that changed must be in `writes`      (else: writes too small)
  - a flag marked '-' (unaffected) must never change  (else: flags wrong)
  - a flag marked '0' / '1' must come out 0 / 1       (else: flags wrong)

'*' and '?' flags are not checked (they may or may not change). Registers
are compared at 8-bit granularity; SP/PC/IFF/I/R are excluded (every
call/ret/push touches SP, and PC always moves).

This cannot prove `writes` is not too LARGE, nor that a '*' flag is really
computed, but it catches the errors that matter for the contract checker:
an instruction that clobbers something the table says it leaves alone.

    python tools/z80table/verify_effects.py [--runs 6] [--jobs 8] [--only ldir]
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import random
import sys
import tempfile
import shutil
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
INSPECCY = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from z80table.runner import Inspeccy  # noqa: E402
from z80table.meta import FLAG_ORDER  # noqa: E402
from z80table.sna import write_sna48  # noqa: E402

TABLE = INSPECCY / "tests" / "refs" / "z80_fingerprint" / "next50.json"


class _Row:
    """Just the fields this check reads, straight from the JSON."""
    def __init__(self, j):
        self.id = j["id"]; self.syntax = j["syntax"]; self.mnemonic = j["mnemonic"]
        self.forms = tuple(j["forms"]); self.bytes = bytes(j["bytes"]); self.canonical = bool(j.get("canonical"))
        self.writes = set(j["writes"]); self.flags = j["flags"]
        t = j["tstates"]; self.t_max = max(t) if isinstance(t, list) else t


def load_rows(path=TABLE):
    import json
    with open(path, encoding="utf-8") as f:
        return [_Row(r) for r in json.load(f)["rows"]]

ORG = 0x8000
STACK = 0xFF00
RET_ADDR = 0x8100

REG8 = {"A": ("AF", 8), "F": ("AF", 0), "B": ("BC", 8), "C": ("BC", 0), "D": ("DE", 8), "E": ("DE", 0),
        "H": ("HL", 8), "L": ("HL", 0), "IXH": ("IX", 8), "IXL": ("IX", 0), "IYH": ("IY", 8), "IYL": ("IY", 0)}
SHADOW = {"AF'": "AFp", "BC'": "BCp", "DE'": "DEp", "HL'": "HLp"}
FLAG_BIT = {"S": 7, "Z": 6, "Y": 5, "H": 4, "X": 3, "P": 2, "N": 1, "C": 0}


def random_state(rng: random.Random) -> dict:
    """Random registers, but pointers stay inside RAM and counters small so
    block instructions terminate and jumps land on NOPs."""
    return dict(
        AF=rng.randrange(0x10000), BC=(rng.randrange(1, 4) << 8) | rng.randrange(0x100),
        DE=0xD000 + rng.randrange(0x800), HL=0xC000 + rng.randrange(0x800),
        IX=0xC800 + rng.randrange(0x400), IY=0xCC00 + rng.randrange(0x400),
        AFp=rng.randrange(0x10000), BCp=rng.randrange(0x10000), DEp=rng.randrange(0x10000), HLp=rng.randrange(0x10000),
        SP=STACK - 2, PC=ORG, I=0x3F, IM=1,
    )


def run_one(isp: Inspeccy, work: Path, row, k: int, regs: dict) -> tuple[dict, dict] | None:
    d = work / f"{row.id}_{k}"
    d.mkdir(parents=True, exist_ok=True)
    ram = bytearray(49152)
    code = bytes(row.bytes)
    ram[ORG - 0x4000:ORG - 0x4000 + len(code)] = code
    ram[STACK - 4 - 0x4000] = RET_ADDR & 0xFF
    ram[STACK - 3 - 0x4000] = RET_ADDR >> 8
    for i in range(0x100):       # something to read at random pointers
        ram[0xC000 - 0x4000 + i] = (i * 37) & 0xFF
    sna = write_sna48(d / "c.sna", regs, ram)
    r = isp.run(sna, workdir=d, machine="next50", cycles=row.t_max + 1, no_interrupts=True,
                trace=True, trace_format="verbose", stem="c")
    lines = isp.parse_trace(r.trace_path)
    shutil.rmtree(d, ignore_errors=True)
    if not lines:
        return None
    # verbose effects column after the FIRST instruction = full register file
    after = {}
    for part in lines[0].effects.split(","):
        part = part.strip()
        if "=" in part:
            k2, v = part.split("=", 1)
            k2 = k2.strip()
            v = v.strip()
            if k2 in ("A", "F"):
                after[k2] = int(v, 16)
            elif k2 in ("AF", "BC", "DE", "HL", "IX", "IY", "SP", "PC", "AF'", "BC'", "DE'", "HL'"):
                after[k2] = int(v, 16)
    return regs, after


def compare(row, before: dict, after: dict) -> list[str]:
    probs = []
    got = {}
    for name, (pair, sh) in REG8.items():
        if pair == "AF":
            src = after.get(name)
            if src is None and "AF" in after:
                src = (after["AF"] >> sh) & 0xFF
        else:
            src = (after[pair] >> sh) & 0xFF if pair in after else None
        if src is None:
            continue
        got[name] = src
    for name, val in got.items():
        pair, sh = REG8[name]
        was = (before[pair] >> sh) & 0xFF
        if val != was and name not in row.writes:
            if name == "F":
                continue  # checked per flag below
            probs.append(f"{row.syntax}: {name} changed {was:#04x}->{val:#04x} but writes={sorted(row.writes)}")
    for sname, dkey in SHADOW.items():
        tk = sname
        if tk in after and after[tk] != before[dkey] and sname not in row.writes:
            probs.append(f"{row.syntax}: {sname} changed but writes={sorted(row.writes)}")
    if "F" in got:
        f0, f1 = before["AF"] & 0xFF, got["F"]
        for fl in FLAG_ORDER:
            code = row.flags.get(fl, "?")
            b = FLAG_BIT[fl]
            v0, v1 = (f0 >> b) & 1, (f1 >> b) & 1
            if code == "-" and v0 != v1:
                probs.append(f"{row.syntax}: flag {fl} marked unaffected but changed {v0}->{v1} (F {f0:#04x}->{f1:#04x})")
            elif code == "0" and v1 != 0:
                probs.append(f"{row.syntax}: flag {fl} marked 0 but is 1 after (F {f0:#04x}->{f1:#04x})")
            elif code == "1" and v1 != 1:
                probs.append(f"{row.syntax}: flag {fl} marked 1 but is 0 after (F {f0:#04x}->{f1:#04x})")
    return probs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=6)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--only", default="", help="substring filter on the syntax")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    isp = Inspeccy()
    rows = [r for r in load_rows() if r.canonical and (not a.only or a.only in r.syntax)]
    # instructions whose effects depend on I/O or that stop the CPU are skipped
    skip = {"halt", "in", "out", "ini", "ind", "inir", "indr", "outi", "outd", "otir", "otdr", "outinb",
            "jp", "reti", "retn", "rst", "nextreg"}
    rows = [r for r in rows if not (r.mnemonic in skip and r.mnemonic != "jp") and not (r.mnemonic == "jp" and r.forms == ("(c)",))]
    rng = random.Random(a.seed)
    work = Path(tempfile.mkdtemp(prefix="inspeccy_fx_"))
    jobs = [(row, k, random_state(rng)) for row in rows for k in range(a.runs)]
    print(f"{len(rows)} rows x {a.runs} random states = {len(jobs)} runs")
    problems: dict[str, list[str]] = {}
    done = 0
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futs = {pool.submit(run_one, isp, work, row, k, regs): row for row, k, regs in jobs}
        for f in cf.as_completed(futs):
            row = futs[f]
            res = f.result()
            done += 1
            if done % 500 == 0:
                print(f"  {done}/{len(jobs)}")
            if res is None:
                continue
            before, after = res
            for p in compare(row, before, after):
                problems.setdefault(p, []).append(row.id)
    shutil.rmtree(work, ignore_errors=True)
    uniq = sorted(problems)
    print(f"\n{len(uniq)} distinct problem(s)")
    for p in uniq:
        print("  " + p)
    return 1 if uniq else 0


if __name__ == "__main__":
    sys.exit(main())
