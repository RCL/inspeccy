"""Extract the Z80/Z80N instruction table from inspeccy + sjasmplus.

No timing or encoding in this file is typed in by hand. For every opcode byte
pattern (base, CB, ED, DD, FD, DDCB, FDCB):

  1. the bytes are placed at $8000 in a 48K snapshot and run for ONE
     instruction under inspeccy (--machine next50, contention off);
  2. inspeccy's own disassembler names the instruction (trace mnemonic column)
     and the trace's cycle stamps give its T-states (next stamp - this stamp);
  3. the run is repeated with different flags / B / BC so conditional and
     repeating instructions expose both paths (taken vs not, repeat vs last);
  4. the disassembler text is assembled back with sjasmplus and the bytes are
     compared: equal -> canonical row the emitter may use; different -> an
     alias encoding (kept, marked, never emitted); refused -> reported.

Flag effects and register reads/writes cannot come from an emulator that
computes rather than tabulates them; they are supplied by meta.py here from
documented references and are then checked empirically by
tools/verify_effects.py.

Usage:  python tools/z80table/extract_table.py [--out tests/refs/z80_fingerprint/<machine>.json]
                                      [--jobs 8] [--machine next50] [--keep]
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
INSPECCY = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from z80table import meta  # noqa: E402
from z80table.runner import Inspeccy  # noqa: E402
from z80table.sna import write_sna48  # noqa: E402

ORG = 0x8000
RET_ADDR = 0x8100          # what the stack holds, so ret/retn/reti land on NOPs
FILL = (0x05, 0x12)        # operand filler: d/n = $05, n2 = $12, nn = $1205 (or $0512 big-endian)
DISP = 0x05                # displacement byte for DDCB
STACK = 0xFF00

# Register state per variant.
#   A: flags clear, B=2, BC=$0205  -> cc false, djnz taken, block repeats continue
#   B: flags set,   B=0, BC=$0001  -> cc true, 16-bit block repeats end (BC=1)
#   C: flags set,   B=1, BC=$0105  -> djnz / inir / otir end (B=1)
# Every instruction runs under all three; a count that differs between them
# is recorded as [longest, shortest] = [taken/repeat, not-taken/last].
VARIANTS = {
    "A": dict(AF=0x1200, BC=0x0205, DE=0xD000, HL=0xC000, IX=0xC100, IY=0xC200, SP=STACK, PC=ORG, I=0x3F, IM=1),
    "B": dict(AF=0x12FF, BC=0x0001, DE=0xD000, HL=0xC000, IX=0xC100, IY=0xC200, SP=STACK, PC=ORG, I=0x3F, IM=1),
    "C": dict(AF=0x12FF, BC=0x0105, DE=0xD000, HL=0xC000, IX=0xC100, IY=0xC200, SP=STACK, PC=ORG, I=0x3F, IM=1),
}

PREFIX_SETS = [
    ((), "base"),
    ((0xCB,), "cb"),
    ((0xED,), "ed"),
    ((0xDD,), "dd"),
    ((0xFD,), "fd"),
    ((0xDD, 0xCB), "ddcb"),
    ((0xFD, 0xCB), "fdcb"),
]

REG8 = {"a", "b", "c", "d", "e", "h", "l", "i", "r", "ixh", "ixl", "iyh", "iyl"}
REG16 = {"af", "bc", "de", "hl", "sp", "ix", "iy", "af'"}
CONDS = {"nz", "z", "nc", "c", "po", "pe", "p", "m"}
INDIRECT_REG = {"(hl)", "(bc)", "(de)", "(sp)", "(c)"}
JUMPS = {"jp", "jr", "call", "ret", "djnz", "rst", "reti", "retn"}

# inspeccy disassembler spelling -> sjasmplus spelling, applied to the whole
# text. Empty until the round trip shows a need; every entry must be justified
# by a sjasmplus refusal, never by taste.
MNEMONIC_ALIASES: dict[str, str] = {}

# Spot checks against documented Z80 / Z80N timings. These are NOT used to
# build the table; they exist to catch an emulator regression like the DD/FD
# prefix double-charge found while writing this tool. Value = T-states, or
# (taken, not-taken).
DOCUMENTED_SPOT_CHECKS = {
    ("nop", ()): 4, ("ld", ("a", "(hl)")): 7, ("ld", ("a", "n")): 7, ("ld", ("hl", "nn")): 10,
    ("ld", ("ix", "nn")): 14, ("ld", ("a", "(ix+d)")): 19, ("ld", ("(ix+d)", "n")): 19,
    ("ld", ("ixh", "n")): 11, ("ld", ("ixh", "b")): 8, ("add", ("ix", "bc")): 15,
    ("inc", ("ix",)): 10, ("inc", ("(ix+d)",)): 23, ("push", ("ix",)): 15, ("pop", ("ix",)): 14,
    ("ld", ("sp", "ix")): 10, ("ex", ("(sp)", "ix")): 23, ("jp", ("(ix)",)): 8,
    ("bit", ("bit", "(ix+d)")): 20, ("set", ("bit", "(ix+d)")): 23, ("rlc", ("(ix+d)",)): 23,
    ("bit", ("bit", "(hl)")): 12, ("set", ("bit", "(hl)")): 15, ("rlc", ("b",)): 8,
    ("ld", ("(nn)", "ix")): 20, ("ld", ("ix", "(nn)")): 20, ("ld", ("(nn)", "hl")): 16,
    ("ld", ("(nn)", "bc")): 20, ("ld", ("a", "(nn)")): 13, ("ld", ("a", "i")): 9,
    ("ldir", ()): (21, 16), ("cpir", ()): (21, 16), ("inir", ()): (21, 16), ("otir", ()): (21, 16),
    ("ldi", ()): 16, ("djnz", ("rel",)): (13, 8), ("jr", ("rel",)): 12, ("jr", ("cc", "rel")): (12, 7),
    ("jp", ("cc", "nn")): 10, ("call", ("cc", "nn")): (17, 10), ("ret", ("cc",)): (11, 5),
    ("ret", ()): 10, ("call", ("nn",)): 17, ("rst", ("rst",)): 11, ("ex", ("(sp)", "hl")): 19,
    ("in", ("a", "(n)")): 11, ("out", ("(n)", "a")): 11, ("in", ("b", "(c)")): 12,
    ("out", ("(c)", "b")): 12, ("rld", ()): 18, ("neg", ()): 8, ("im", ("im",)): 8,
    ("reti", ()): 14, ("add", ("hl", "bc")): 11, ("adc", ("hl", "bc")): 15, ("ld", ("sp", "hl")): 6,
    ("mul", ("d", "e")): 8, ("add", ("hl", "a")): 8, ("add", ("hl", "nn")): 16,
    ("push", ("nn",)): 23, ("nextreg", ("n", "n")): 20, ("nextreg", ("n", "a")): 17,
    ("swapnib", ()): 8, ("mirror", ("a",)): 8, ("test", ("n",)): 11, ("bsla", ("de", "b")): 8,
    ("ldix", ()): 16, ("ldirx", ()): (21, 16), ("ldws", ()): 14, ("pixelad", ()): 8,
    ("setae", ()): 8, ("jp", ("(c)",)): 13, ("outinb", ()): 16, ("ldpirx", ()): (21, 16),
}


# ---------------------------------------------------------------------------
# Disassembly text -> operand forms


class ParseFail(Exception):
    pass


_IXD = re.compile(r"^\((ix|iy)([+-])\$([0-9a-f]{1,2})\)$")
_IND16 = re.compile(r"^\(\$([0-9a-f]{4})\)$")
_IND8 = re.compile(r"^\(\$([0-9a-f]{2})\)$")
_IMM16 = re.compile(r"^\$([0-9a-f]{4})$")
_IMM8 = re.compile(r"^\$([0-9a-f]{2})$")
_DEC = re.compile(r"^\d+$")


def parse_text(text: str) -> tuple[str, list[tuple[str, int | None]]]:
    """'ld (ix+$05), $12' -> ('ld', [('(ix+d)', 5), ('n', 0x12)])"""
    text = text.strip().lower()
    if not text or text.startswith("db ") or text.startswith("invalid"):
        raise ParseFail("invalid/undefined opcode")
    parts = text.split(None, 1)
    mnem = parts[0]
    ops = [o.strip() for o in parts[1].split(",")] if len(parts) > 1 else []
    ops = [o for o in ops if o != ""]
    out: list[tuple[str, int | None]] = []
    for i, s in enumerate(ops):
        if s in REG8 or s in REG16 or s in INDIRECT_REG:
            if s in CONDS and mnem in ("jp", "jr", "call", "ret") and i == 0 and (len(ops) == 2 or mnem == "ret"):
                out.append(("cc", s))  # type: ignore[arg-type]
            else:
                out.append((s, None))
            continue
        if s in CONDS and mnem in ("jp", "jr", "call", "ret") and i == 0:
            out.append(("cc", s))  # type: ignore[arg-type]
            continue
        if s in ("(ix)", "(iy)"):
            out.append((s, None))
            continue
        m = _IXD.match(s)
        if m:
            v = int(m.group(3), 16) * (1 if m.group(2) == "+" else -1)
            out.append((f"({m.group(1)}+d)", v))
            continue
        m = _IND16.match(s)
        if m:
            out.append(("(nn)", int(m.group(1), 16)))
            continue
        m = _IND8.match(s)
        if m:
            out.append(("(n)", int(m.group(1), 16)))
            continue
        m = _IMM16.match(s)
        if m:
            v = int(m.group(1), 16)
            out.append(("rel", v) if mnem in ("jr", "djnz") else ("nn", v))
            continue
        m = _IMM8.match(s)
        if m:
            v = int(m.group(1), 16)
            if mnem == "rst":
                out.append(("rst", v))
            elif mnem in ("jr", "djnz"):
                out.append(("rel", v))
            else:
                out.append(("n", v))
            continue
        if _DEC.match(s):
            v = int(s)
            if mnem in ("bit", "res", "set"):
                out.append(("bit", v))
            elif mnem == "im":
                out.append(("im", v))
            elif mnem == "rst":
                out.append(("rst", v))
            elif mnem == "out" and v == 0:
                out.append(("0", None))
            else:
                out.append(("n", v))
            continue
        raise ParseFail(f"unrecognised operand {s!r} in {text!r}")
    return mnem, out


FIXED_TOKENS = ("cc", "bit", "im", "rst")   # operand values encoded in the opcode byte itself


def render_syntax(mnem: str, ops: list[tuple[str, int | None]]) -> str:
    parts = []
    for t, v in ops:
        if t == "cc":
            parts.append(str(v))
        elif t in ("bit", "im"):
            parts.append(str(v))
        elif t == "rst":
            parts.append(f"${v:02X}")
        else:
            parts.append(t)
    return (mnem + " " + ",".join(parts)).strip()


def render_concrete(mnem: str, ops: list[tuple[str, int | None]]) -> str:
    """The row with its filler values, in sjasmplus syntax."""
    parts = []
    for t, v in ops:
        if t == "cc":
            parts.append(str(v))
        elif t in ("(ix+d)", "(iy+d)"):
            parts.append(f"({t[1:3]}{v:+d})")
        elif t == "(nn)":
            parts.append(f"(${v:04X})")
        elif t == "(n)":
            parts.append(f"(${v:02X})")
        elif t == "nn":
            parts.append(f"${v:04X}")
        elif t == "n":
            parts.append(f"${v:02X}")
        elif t == "rel":
            parts.append(f"${v:04X}")
        elif t in ("bit", "im"):
            parts.append(str(v))
        elif t == "rst":
            parts.append(f"${v:02X}")
        else:
            parts.append(t)
    return (mnem + " " + ",".join(parts)).strip()


# ---------------------------------------------------------------------------
# One emulator run


def run_candidate(isp: Inspeccy, machine: str, work: Path, key: str, code: bytes, variant: str) -> dict:
    d = work / f"{key}_{variant}"
    d.mkdir(parents=True, exist_ok=True)
    ram = bytearray(49152)
    ram[ORG - 0x4000:ORG - 0x4000 + len(code)] = code
    # return address on the stack (below the SNA's own PC push)
    ram[STACK - 4 - 0x4000] = RET_ADDR & 0xFF
    ram[STACK - 3 - 0x4000] = RET_ADDR >> 8
    regs = dict(VARIANTS[variant])
    regs["SP"] = STACK - 2  # write_sna48 pushes PC below this -> SP = STACK-4 at run time
    sna = write_sna48(d / "c.sna", regs, ram)
    r = isp.run(sna, workdir=d, machine=machine, cycles=400, no_interrupts=True,
                trace=True, trace_format="minimal", stem="c")
    lines = isp.parse_trace(r.trace_path)
    res = {"lines": [(ln.cycles, ln.addr, ln.mnemonic) for ln in lines[:3]],
           "result": r.result_name, "cycles": int(r.summary.get("cycles", "0"))}
    shutil.rmtree(d, ignore_errors=True)
    return res


def measure(res: dict) -> int | None:
    lines = res["lines"]
    if len(lines) >= 2:
        return lines[1][0] - lines[0][0]
    if len(lines) == 1 and res["result"] in ("RestartTrap", "InvalidOpcode"):
        return res["cycles"] - lines[0][0]
    return None


# ---------------------------------------------------------------------------
# sjasmplus round trip (all rows in one file)


def sjasm_roundtrip(rows: list[dict], work: Path) -> None:
    src = work / "rt.asm"
    lst = work / "rt.lst"
    lines = ["    DEVICE ZXSPECTRUMNEXT"]   # Z80N mnemonics are refused without it
    line_of_row: dict[int, int] = {}
    addr_of_row: dict[int, int] = {}
    for i, row in enumerate(rows):
        org = 0x8000 + i * 16
        mnem, ops = row["mnemonic"], [(t, v) for t, v in row["_ops"]]
        # relative targets were measured at ORG; rebase to this row's org
        ops2 = [(t, (v - ORG + org) if t == "rel" else v) for t, v in ops]
        text = render_concrete(mnem, ops2)
        for a, b in MNEMONIC_ALIASES.items():
            if text.startswith(a + " ") or text == a:
                text = b + text[len(a):]
        lines.append(f"    org ${org:04X}")
        lines.append(f"    {text}")
        line_of_row[len(lines)] = i
        addr_of_row[i] = org
    src.write_text("\n".join(lines) + "\n", encoding="utf-8")
    p = subprocess.run(["sjasmplus", "--nologo", "--msg=err", f"--lst={lst}", str(src)],
                       capture_output=True, text=True)
    errs: dict[int, str] = {}
    for m in re.finditer(r"rt\.asm\((\d+)\): error: (.*)", p.stdout + p.stderr):
        ln = int(m.group(1))
        if ln in line_of_row:
            errs[line_of_row[ln]] = m.group(2).strip()
    got: dict[int, bytes] = {}
    if lst.exists():
        for line in lst.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"^\s*\d+\+?\s+([0-9A-F]{4})\s+((?:[0-9A-F]{2}\s)+)", line + " ")
            if m:
                addr = int(m.group(1), 16)
                got.setdefault(addr, b"")
                got[addr] += bytes.fromhex(m.group(2).replace(" ", ""))
    by_bytes = {bytes(r["bytes"]): r["id"] for r in rows}
    for i, row in enumerate(rows):
        if i in errs:
            row["canonical"] = False
            row["sjasm_error"] = errs[i]
            continue
        b = got.get(addr_of_row[i])
        if b is None:
            row["canonical"] = False
            row["sjasm_error"] = "no bytes in listing"
            continue
        row["sjasm_bytes"] = list(b)
        if b == bytes(row["bytes"]):
            row["canonical"] = True
        else:
            row["canonical"] = False
            row["alias_of"] = by_bytes.get(b)


# ---------------------------------------------------------------------------


def build_template(row: dict) -> None:
    """Replace operand bytes in `bytes` with placeholders naming the operand."""
    b = list(row["bytes"])
    n_pre = row["_opcode_end"]           # index of first operand byte
    tmpl: list = b[:n_pre]
    tail = b[n_pre:]
    ops = row["_ops"]
    # DDCB: the displacement sits BEFORE the opcode byte
    if row["_set"] in ("ddcb", "fdcb"):
        idx = next(i for i, (t, _) in enumerate(ops) if t in ("(ix+d)", "(iy+d)"))
        tmpl = [b[0], b[1], f"op{idx}", b[3]]
        row["template"] = tmpl
        return
    pos = 0
    for i, (t, v) in enumerate(ops):
        if t in ("n", "(n)"):
            if pos >= len(tail) or tail[pos] != (v & 0xFF):
                raise ParseFail(f"operand {i} ({t}={v}) does not match byte {tail[pos:pos+1]}")
            tmpl.append(f"op{i}")
            pos += 1
        elif t in ("(ix+d)", "(iy+d)"):
            if pos >= len(tail) or tail[pos] != (v & 0xFF):
                raise ParseFail(f"displacement {v} does not match byte {tail[pos:pos+1]}")
            tmpl.append(f"op{i}")
            pos += 1
        elif t in ("nn", "(nn)"):
            if pos + 1 >= len(tail):
                raise ParseFail("16-bit operand but fewer than 2 operand bytes")
            lo, hi = tail[pos], tail[pos + 1]
            if v == lo | (hi << 8):
                tmpl += [f"op{i}.lo", f"op{i}.hi"]
            elif v == hi | (lo << 8):
                tmpl += [f"op{i}.hi", f"op{i}.lo"]
                row["big_endian"] = True
            else:
                raise ParseFail(f"16-bit operand {v:#06x} matches neither byte order of {lo:#04x} {hi:#04x}")
            pos += 2
        elif t == "rel":
            if pos >= len(tail):
                raise ParseFail("relative operand but no operand byte")
            rel = (v - (ORG + row["size"])) & 0xFF
            if tail[pos] != rel:
                raise ParseFail(f"relative target {v:#06x} implies byte {rel:#04x}, found {tail[pos]:#04x}")
            tmpl.append(f"op{i}.rel")
            pos += 1
    if pos != len(tail):
        raise ParseFail(f"{len(tail) - pos} operand byte(s) unclaimed by any operand")
    row["template"] = tmpl


def display_form(forms: list[str]) -> str:
    m = {"b": "r", "c": "r", "d": "r", "e": "r", "h": "r", "l": "r", "a": "r"}
    return ",".join(m.get(f, f) for f in forms)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=None, help="default: tests/refs/z80_fingerprint/<machine>.json")
    ap.add_argument("--jobs", type=int, default=max(2, min(8, (os.cpu_count() or 4))))
    ap.add_argument("--machine", default="next50")
    ap.add_argument("--inspeccy", default=None)
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    ap.add_argument("--limit", type=int, default=0, help="debug: only the first N candidates")
    args = ap.parse_args()

    if args.out is None:
        args.out = str(INSPECCY / "tests" / "refs" / "z80_fingerprint" / f"{args.machine}.json")
    isp = Inspeccy(args.inspeccy)
    print(f"inspeccy: {isp.exe}")
    sjp = subprocess.run(["sjasmplus", "--version"], capture_output=True, text=True); sj = (sjp.stdout + sjp.stderr).strip().splitlines()
    sj_version = sj[0] if sj else "?"
    print(f"sjasmplus: {sj_version}")

    work = Path(tempfile.mkdtemp(prefix="inspeccy_extract_"))
    print(f"work dir: {work}")

    # ---- enumerate candidates
    cands: list[tuple[str, str, bytes, int]] = []   # key, set, bytes, opcode_end
    for prefix, name in PREFIX_SETS:
        for op in range(256):
            if not prefix and op in (0xCB, 0xED, 0xDD, 0xFD):
                continue
            if prefix in ((0xDD,), (0xFD,)) and op in (0xCB, 0xED, 0xDD, 0xFD):
                continue
            if name in ("ddcb", "fdcb"):
                code = bytes(prefix) + bytes([DISP, op])
                opcode_end = 4
            else:
                code = bytes(prefix) + bytes([op])
                opcode_end = len(code)
            code = code + bytes(FILL)
            key = f"{name}_{op:02x}"
            cands.append((key, name, code, opcode_end))
    if args.limit:
        cands = cands[:args.limit]
    print(f"{len(cands)} byte patterns x {len(VARIANTS)} variants")

    # ---- run
    results: dict[tuple[str, str], dict] = {}
    todo = [(c, v) for c in cands for v in VARIANTS]
    done = 0
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_candidate, isp, args.machine, work, c[0], c[2], v): (c, v) for c, v in todo}
        for f in cf.as_completed(futs):
            c, v = futs[f]
            results[(c[0], v)] = f.result()
            done += 1
            if done % 400 == 0:
                print(f"  {done}/{len(todo)} runs")

    # ---- build rows
    rows: list[dict] = []
    skipped: list[str] = []
    problems: list[str] = []
    for key, setname, code, opcode_end in cands:
        rv = {v: results[(key, v)] for v in VARIANTS}
        ra = rv["A"]
        if not ra["lines"]:
            skipped.append(f"{key}: no trace line (result {ra['result']})")
            continue
        text = ra["lines"][0][2]
        try:
            mnem, ops = parse_text(text)
        except ParseFail as e:
            if "invalid" in str(e):
                skipped.append(f"{key}: {text}")
            else:
                problems.append(f"{key}: {e}")
            continue
        for v, r in rv.items():
            if r["lines"] and r["lines"][0][2] != text:
                problems.append(f"{key}: disassembly differs between variants A/{v}: {text!r} vs {r['lines'][0][2]!r}")
        ts = [t for t in (measure(r) for r in rv.values()) if t is not None]
        if not ts:
            if mnem == "halt":
                ts = [4]
                timing_source = "documented (inspeccy fast-forwards HALT; not measurable)"
            else:
                problems.append(f"{key}: {text}: could not measure T-states ({ra['result']})")
                continue
        else:
            timing_source = "inspeccy"
        ta, tb = max(ts), min(ts)
        # operand byte count from the text
        nbytes = 0
        for t, _ in ops:
            nbytes += {"n": 1, "(n)": 1, "rel": 1, "nn": 2, "(nn)": 2}.get(t, 0)
            if t in ("(ix+d)", "(iy+d)") and setname not in ("ddcb", "fdcb"):
                nbytes += 1
        size = opcode_end + nbytes
        # cross-check with PC delta when the instruction fell through
        nxt = ra["lines"][1][1] if len(ra["lines"]) > 1 else None
        if nxt is not None and 0 < nxt - ORG <= 6 and mnem not in JUMPS and nxt - ORG != size:
            problems.append(f"{key}: {text}: size from text {size} but PC advanced {nxt - ORG}")
            continue
        forms = [t for t, _ in ops]
        row = {
            "id": key,
            "set": setname,
            "mnemonic": mnem,
            "forms": forms,
            "fixed": {str(i): v for i, (t, v) in enumerate(ops) if t in FIXED_TOKENS},
            "syntax": render_syntax(mnem, ops),
            "disasm": text,
            "bytes": list(code[:size]),
            "size": size,
            "m1": (2 if setname in ("ddcb", "fdcb") else opcode_end),
            "tstates": ta if ta == tb else [max(ta, tb), min(ta, tb)],
            "timing_source": timing_source,
            "_ops": ops,
            "_set": setname,
            "_opcode_end": opcode_end,
        }
        if ta != tb:
            row["tstates_note"] = "[taken/repeat, not-taken/last]"
        try:
            build_template(row)
        except ParseFail as e:
            problems.append(f"{key}: {text}: {e}")
            continue
        rows.append(row)

    # ---- sjasmplus leg
    sjasm_roundtrip(rows, work)

    # ---- effects metadata (documented) and spot checks
    missing_meta: set[str] = set()
    spot_fail: list[str] = []
    for row in rows:
        try:
            eff = meta.effects(row["mnemonic"], row["forms"])
        except meta.NoRule as e:
            missing_meta.add(str(e))
            continue
        except Exception as e:  # noqa: BLE001 - a rule that crashes is a missing rule
            missing_meta.add(f"{row['syntax']}: {type(e).__name__}: {e}")
            continue
        row["flags"] = eff["flags"]
        row["reads"] = sorted(eff["reads"])
        row["writes"] = sorted(eff["writes"])
        row["mem"] = eff["mem"]
        if row.get("canonical"):
            k = (row["mnemonic"], tuple(display_form(row["forms"]).split(",")) if row["forms"] else ())
            want = DOCUMENTED_SPOT_CHECKS.get(k)
            if want is not None:
                got = row["tstates"]
                got_t = tuple(got) if isinstance(got, list) else got
                if got_t != want:
                    spot_fail.append(f"{row['syntax']}: inspeccy {got_t} vs documented {want}")

    for row in rows:
        for k in list(row):
            if k.startswith("_"):
                del row[k]
    rows.sort(key=lambda r: (PREFIX_SETS.index(next(p for p in PREFIX_SETS if p[1] == r["set"])), r["bytes"]))

    n_can = sum(1 for r in rows if r.get("canonical"))
    n_alias = sum(1 for r in rows if r.get("alias_of") is not None)
    n_err = sum(1 for r in rows if r.get("sjasm_error"))
    print(f"\nrows: {len(rows)}  canonical: {n_can}  alias encodings: {n_alias}  sjasmplus refused: {n_err}")
    print(f"invalid/undefined patterns skipped: {len(skipped)}")
    if n_err:
        print("\nsjasmplus refused (these rows are unusable until MNEMONIC_ALIASES covers them):")
        for r in rows:
            if r.get("sjasm_error"):
                print(f"  {r['id']:8} {r['disasm']:28} {r['sjasm_error']}")
    if problems:
        print("\nPROBLEMS (rows dropped):")
        for p in problems:
            print("  " + p)
    if missing_meta:
        print("\nMISSING EFFECT RULES in tools/z80table/meta.py:")
        for m in sorted(missing_meta):
            print("  " + m)
    if spot_fail:
        print("\nDOCUMENTED-TIMING SPOT CHECKS FAILED (inspeccy disagrees with the Z80 manual):")
        for s in spot_fail:
            print("  " + s)

    out = {
        "meta": {
            "generated": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "generator": "tools/z80table/extract_table.py",
            "inspeccy": str(isp.exe),
            "machine": args.machine,
            "sjasmplus": sj_version,
            "timing_source": "inspeccy trace cycle stamps, contention off",
            "flags_source": "tools/z80table/meta.py (documented Z80 / Z80N references), verified by tools/z80table/verify_effects.py",
            "spot_check_failures": spot_fail,
        },
        "rows": rows,
    }
    outp = Path(args.out)
    outp.parent.mkdir(parents=True, exist_ok=True)
    outp.write_text(json.dumps(out, indent=1), encoding="utf-8")
    print(f"\nwrote {outp} ({outp.stat().st_size} bytes)")
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    bad = bool(problems or missing_meta or spot_fail or n_err)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
