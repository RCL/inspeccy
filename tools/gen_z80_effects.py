#!/usr/bin/env python3
"""Generate the Z80/Z80N effects header the sjasmplus fork's contract checker
compiles in, from the bundled opcode table.

One source: tests/refs/z80_fingerprint/next50.json. Its encodings and timings
were observed by running every opcode in inspeccy; its register and flag
effects come from documentation (tools/z80table/meta.py) and are checked
empirically by tools/z80table/verify_effects.py. Nothing here is typed in by
hand, and the header is never edited: regenerate it when the table changes.

    python tools/gen_z80_effects.py --out path/to/z80_effects_generated.h
    python tools/gen_z80_effects.py --out path/to/z80_effects_generated.h --check

The header is seven 256-entry arrays, one per opcode page (base, CB, ED, DD,
FD, DDCB, FDCB), indexed by the opcode byte after the prefix. Each entry
carries what the checker needs to follow a routine's dataflow: the registers
it reads and writes as bitmasks, the flags it writes, its T-states (taken
and not-taken), its size, and a few shape bits (ends a basic block, stores
to an absolute address, has a relative or absolute branch operand).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
INSPECCY = TOOLS.parent
TABLE = INSPECCY / "tests" / "refs" / "z80_fingerprint" / "next50.json"

# register bit assignment, shared with the checker (it names them by these
# indices, so the order is part of the contract between the two files)
REGS = ["A", "F", "B", "C", "D", "E", "H", "L",
        "IXH", "IXL", "IYH", "IYL", "SP", "PC", "I", "R", "IFF",
        "AF'", "BC'", "DE'", "HL'"]
REG_BIT = {name: 1 << i for i, name in enumerate(REGS)}
FLAG_ORDER = "SZYHXPNC"          # bit 7 .. bit 0 of F, as the Z80 lays them out
PAGES = ["base", "cb", "ed", "dd", "fd", "ddcb", "fdcb"]

# shape bits
SH_ENDS_BLOCK = 1      # writes PC: jump, call, ret, djnz, rst, halt is not one
SH_COND = 2            # conditional (has taken / not-taken timings)
SH_REL = 4             # has a relative branch operand (jr, djnz)
SH_ABS_BRANCH = 8      # has an absolute branch operand (jp nn, call nn)
SH_ABS_STORE = 16      # stores to an absolute address: ld (nn),r / ld (nn),rr
SH_ABS_LOAD = 32       # loads from an absolute address
SH_CALL = 64           # pushes a return address (call, rst)
SH_RET = 128           # pops one (ret, reti, retn)
SH_EXX = 256           # exx
SH_EXAF = 512          # ex af,af'
SH_INDIRECT_JP = 1024  # jp (hl) / jp (ix) / jp (iy) / jp (c)
SH_BLOCK_REPEAT = 2048 # ldir and friends: repeats until BC (or B) is spent


def load():
    if not TABLE.is_file():
        raise SystemExit(f"bundled opcode table not found: {TABLE}")
    j = json.loads(TABLE.read_text(encoding="utf-8"))
    return j["meta"], j["rows"]


def opcode_index(row):
    """(page, index) for the row, from its byte pattern."""
    b = row["bytes"]
    page = row["set"]
    if page == "base":
        return page, b[0]
    if page in ("cb", "ed"):
        return page, b[1]
    if page in ("dd", "fd"):
        return page, b[1]
    if page in ("ddcb", "fdcb"):
        return page, b[3]
    raise SystemExit(f"unknown set {page!r} in {row['id']}")


def flags_written(row):
    m = 0
    for i, name in enumerate(FLAG_ORDER):
        if row["flags"][name] != "-":
            m |= 1 << (7 - i)
    return m


def reg_mask(names):
    m = 0
    for n in names:
        if n not in REG_BIT:
            raise SystemExit(f"register {n!r} is not in REGS")
        m |= REG_BIT[n]
    return m


def shape(row):
    syn = row["syntax"]
    mn = row["mnemonic"]
    tpl = row["template"]
    tstr = [t for t in tpl if isinstance(t, str)]
    s = 0
    if "PC" in row["writes"]:
        s |= SH_ENDS_BLOCK
    if isinstance(row["tstates"], list) and mn not in ("ldir", "lddr", "cpir", "cpdr", "inir", "indr", "otir", "otdr",
                                                        "ldirx", "lddrx", "ldpirx", "ldirscale"):
        s |= SH_COND
    if any(t.endswith(".rel") for t in tstr):
        s |= SH_REL
    if mn in ("jp", "call") and any(t.endswith(".lo") for t in tstr):
        s |= SH_ABS_BRANCH
    if mn == "ld" and "w" in row["mem"] and any(t.endswith(".lo") for t in tstr) and syn.startswith("ld ("):
        s |= SH_ABS_STORE
    if mn == "ld" and "r" in row["mem"] and any(t.endswith(".lo") for t in tstr) and not syn.startswith("ld ("):
        s |= SH_ABS_LOAD
    if mn in ("call", "rst"):
        s |= SH_CALL
    if mn in ("ret", "reti", "retn"):
        s |= SH_RET
    if mn == "exx":
        s |= SH_EXX
    if syn == "ex af,af'":
        s |= SH_EXAF
    if mn == "jp" and syn.startswith("jp ("):
        s |= SH_INDIRECT_JP
    if mn in ("ldir", "lddr", "cpir", "cpdr", "inir", "indr", "otir", "otdr", "ldirx", "lddrx", "ldpirx", "ldirscale"):
        s |= SH_BLOCK_REPEAT
    return s


def render(meta, rows):
    by_page = {p: [None] * 256 for p in PAGES}
    for r in rows:
        page, idx = opcode_index(r)
        cur = by_page[page][idx]
        # a canonical row wins over an alias with the same encoding; aliases
        # (undocumented duplicates) have the same effects anyway
        if cur is None or (r.get("canonical") and not cur.get("canonical")):
            by_page[page][idx] = r

    L = []
    W = L.append
    W("#pragma once")
    W("// Generated Z80/Z80N effects table; regenerate with")
    W("// inspeccy/source/tools/gen_z80_effects.py. Never edit by hand.")
    W(f"// Source: tests/refs/z80_fingerprint/next50.json, generated {meta.get('generated')}")
    W("// (timing observed in inspeccy, effects documented and empirically verified).")
    W("")
    W("#include <stdint.h>")
    W("")
    W("namespace Z80Fx {")
    W("")
    W("\t// register bits of OpFx::reads / OpFx::writes")
    for i, name in enumerate(REGS):
        ident = name.replace("'", "_ALT")
        W(f"\tconstexpr uint32_t R_{ident} = 1u << {i};")
    W("\tconstexpr uint32_t R_AF = R_A | R_F;")
    W("\tconstexpr uint32_t R_BC = R_B | R_C;")
    W("\tconstexpr uint32_t R_DE = R_D | R_E;")
    W("\tconstexpr uint32_t R_HL = R_H | R_L;")
    W("\tconstexpr uint32_t R_IX = R_IXH | R_IXL;")
    W("\tconstexpr uint32_t R_IY = R_IYH | R_IYL;")
    W("\tconstexpr uint32_t R_SHADOW = R_AF_ALT | R_BC_ALT | R_DE_ALT | R_HL_ALT;")
    W("")
    W("\t// flag bits of OpFx::flagsWritten, as the Z80 lays F out")
    for i, name in enumerate(FLAG_ORDER):
        W(f"\tconstexpr uint8_t F_{name} = 1u << {7 - i};")
    W("")
    W("\t// shape bits")
    for name, val in [("SH_ENDS_BLOCK", SH_ENDS_BLOCK), ("SH_COND", SH_COND), ("SH_REL", SH_REL),
                      ("SH_ABS_BRANCH", SH_ABS_BRANCH), ("SH_ABS_STORE", SH_ABS_STORE), ("SH_ABS_LOAD", SH_ABS_LOAD),
                      ("SH_CALL", SH_CALL), ("SH_RET", SH_RET), ("SH_EXX", SH_EXX), ("SH_EXAF", SH_EXAF),
                      ("SH_INDIRECT_JP", SH_INDIRECT_JP), ("SH_BLOCK_REPEAT", SH_BLOCK_REPEAT)]:
        W(f"\tconstexpr uint16_t {name} = {val};")
    W("")
    W("\tstruct OpFx {")
    W("\t\tconst char* syntax;\t\t// canonical form, nullptr for an undefined opcode")
    W("\t\tuint32_t reads;\t\t\t// registers read (F here means the flags are read)")
    W("\t\tuint32_t writes;\t\t// registers written (F here means some flag is written)")
    W("\t\tuint8_t flagsWritten;\t// which flags, when F is written")
    W("\t\tuint8_t size;\t\t\t// instruction length in bytes")
    W("\t\tuint16_t tstates;\t\t// taken / repeat")
    W("\t\tuint16_t tstatesAlt;\t// not taken / last; == tstates when unconditional")
    W("\t\tuint16_t shape;\t\t\t// SH_* bits")
    W("\t};")
    W("")
    W("\tenum EPage { PAGE_BASE = 0, PAGE_CB, PAGE_ED, PAGE_DD, PAGE_FD, PAGE_DDCB, PAGE_FDCB, PAGE_COUNT };")
    W("")
    for p in PAGES:
        W(f"\tconstexpr OpFx {p}[256] = {{")
        for idx in range(256):
            r = by_page[p][idx]
            if r is None:
                W(f"\t\t{{ nullptr, 0, 0, 0, 0, 0, 0, 0 }},\t// {idx:02x}")
                continue
            ts = r["tstates"]
            tt, ta = (ts[0], ts[1]) if isinstance(ts, list) else (ts, ts)
            syn = r["syntax"].replace("\\", "\\\\").replace('"', '\\"')
            W(f'\t\t{{ "{syn}", 0x{reg_mask(r["reads"]):06x}, 0x{reg_mask(r["writes"]):06x}, '
              f'0x{flags_written(r):02x}, {r["size"]}, {tt}, {ta}, {shape(r)} }},\t// {idx:02x}')
        W("\t};")
        W("")
    W("\tconstexpr const OpFx* pages[PAGE_COUNT] = { base, cb, ed, dd, fd, ddcb, fdcb };")
    W("")
    W("} // namespace Z80Fx")
    W("")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="exit 1 if the committed header is stale")
    ap.add_argument("--out", type=Path, required=True, help="destination effects header")
    a = ap.parse_args()
    meta, rows = load()
    text = render(meta, rows)
    if a.check:
        cur = a.out.read_text(encoding="utf-8") if a.out.is_file() else ""
        if cur != text:
            print(f"STALE: {a.out} does not match a fresh generation; run tools/gen_z80_effects.py")
            return 1
        print(f"ok: {a.out.name} is up to date")
        return 0
    a.out.write_text(text, encoding="utf-8")
    print(f"wrote {a.out} ({len(text)} bytes, {text.count(chr(10))} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
