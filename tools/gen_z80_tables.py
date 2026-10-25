#!/usr/bin/env python3
"""Generate the C++ opcode metadata from the bundled frozen Next reference.

Run with --check to detect a stale header. The frozen table is a regression
reference derived from this emulator, not an independent hardware oracle.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
INSPECCY = TOOLS.parent
TABLE = INSPECCY / "tests" / "refs" / "z80_fingerprint" / "next50.json"
HEADER = INSPECCY / "core" / "src" / "z80_tables_generated.h"

# Peephole's per-class costs, each pinned to one representative canonical row.
# (constant base name) -> (row syntax, human comment)
PEEPHOLE_CLASSES = [
    ("LdRegImm",   "ld b,n",     "ld r,n"),
    ("LdRegReg",   "ld b,c",     "ld r,r'"),
    ("IncDecReg",  "inc b",      "inc r / dec r"),
    ("LdPairImm",  "ld bc,nn",   "ld rr,nn"),
    ("IncDecPair", "inc bc",     "inc rr / dec rr"),
    ("LogicalA",   "or a",       "or a / and a / xor a"),
    ("Call",       "call nn",    "call nn"),
    ("Ret",        "ret",        "ret"),
    ("Jp",         "jp nn",      "jp nn"),
]


def load_rows() -> dict:
    if not TABLE.is_file():
        raise SystemExit(f"bundled opcode reference not found: {TABLE}")
    j = json.loads(TABLE.read_text(encoding="utf-8"))
    return j, {r["syntax"]: r for r in j["rows"] if r.get("canonical")}


def t_taken(ts) -> int:
    return ts[0] if isinstance(ts, list) else ts


def render(meta: dict, by_syntax: dict) -> str:
    L = []
    W = L.append
    W("#pragma once")
    W("// Generated opcode metadata; regenerate with tools/gen_z80_tables.py.")
    W("")
    W("#include <stdint.h>")
    W("")
    W("namespace z80gen")
    W("{")
    W("")
    W("// Representative instruction costs used by the peephole model.")
    for base, syntax, comment in PEEPHOLE_CLASSES:
        r = by_syntax.get(syntax)
        if r is None:
            raise SystemExit(f"peephole class {base}: no canonical row for {syntax!r}")
        t = t_taken(r["tstates"])
        W(f"constexpr uint32_t kT_{base} = {t}; // {comment} ({syntax}, from {r['id']})")
        W(f"constexpr uint32_t kB_{base} = {r['size']}; // bytes")
    W("")
    # Full canonical metadata table (the reviewable single source; the seed for
    # further consumers). One row per canonical (mnemonic, operand-form).
    rows = [r for r in meta["rows"] if r.get("canonical")]
    rows.sort(key=lambda r: (bytes(r["bytes"]), r["id"]))
    W("struct OpMeta")
    W("{")
    W("\tconst char* m_Id; // stable row id, e.g. \"base_7e\"")
    W("\tconst char* m_Syntax; // canonical form, e.g. \"ld a,(hl)\"")
    W("\tuint8_t m_Bytes[4]; // encoding (opcode + operand placeholders as filler)")
    W("\tuint8_t m_Size; // instruction length")
    W("\tuint8_t m_M1; // M1 fetch cycles (2 for CB/ED/DD/FD-prefixed opcode fetch pairs)")
    W("\tuint16_t m_TStates; // base (taken/repeat) T-states")
    W("\tuint16_t m_TStatesAlt; // not-taken/last T-states (== TStates when unconditional)")
    W("};")
    W("")
    W(f"constexpr OpMeta kOpMeta[{len(rows)}] = {{")
    for r in rows:
        b = list(r["bytes"])[:4] + [0, 0, 0, 0]
        bb = ", ".join(f"0x{x:02X}" for x in b[:4])
        ts = r["tstates"]
        tt = t_taken(ts)
        ta = ts[1] if isinstance(ts, list) else tt
        syn = r["syntax"].replace("\\", "\\\\").replace('"', '\\"')
        W(f'\t{{"{r["id"]}", "{syn}", {{{bb}}}, {r["size"]}, {r["m1"]}, {tt}, {ta}}},')
    W("};")
    W("")
    W("} // namespace z80gen")
    W("")
    return "\n".join(L)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="exit 1 if the committed header is stale")
    a = ap.parse_args()
    meta, by_syntax = load_rows()
    text = render(meta, by_syntax)
    if a.check:
        cur = HEADER.read_text(encoding="utf-8") if HEADER.is_file() else ""
        if cur != text:
            print(f"STALE: {HEADER} does not match a fresh generation; run tools/gen_z80_tables.py")
            return 1
        print(f"ok: {HEADER.name} is up to date")
        return 0
    HEADER.write_text(text, encoding="utf-8")
    print(f"wrote {HEADER} ({len(text)} bytes, {text.count(chr(10))} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
