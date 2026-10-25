#!/usr/bin/env python3
"""Dump/resume round-trip for ZX-Evo BaseConf and ZX-Uno state.

For each machine a program sets its video state (Evo: $EFF7 HW-multicolour + a
page-window override; Uno: reg $0B turbo + OUT 255,2 SCLD mode) plus 8x1
attributes (line 0 bright-red, line 1 bright-green) and HALTs.  The run is
dumped (--dump-out); we assert the machine-specific fields are present, then
--resume the dump and render.  A correct resume re-arms the machine and restores
$EFF7 / scld_mode, so the 8x1 render survives — line 1 is green.  If resume did
NOT re-arm, the mode would fall back to standard 8x8 and line 1 would inherit
line 0's red.  Exit codes: 0 pass, 1 fail.
"""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from test_zxevo_baseconf import (BIN_CANDIDATES, ROM128, BORDER,
                                 IDX_RED_BRIGHT, IDX_GREEN_BRIGHT,
                                 fill_loop, build_sna_128k, bmp_index_at,
                                 find_inspeccy)


def attrs_and_halt() -> bytes:
    return (fill_loop(0x4000, 0xFF) + fill_loop(0x4100, 0xFF) +
            fill_loop(0x6000, 0x42) + fill_loop(0x6100, 0x44) +
            b"\x76")                                  # halt (di'd -> exit 2)


def evo_program() -> bytes:
    return (b"\xF3"                                   # di
            b"\x3E\x20\x01\xF7\xEF\xED\x79"           # ld a,$20; ld bc,$EFF7; out(c),a
            b"\x3E\xEB\x01\xF7\xF7\xED\x79"           # ld a,$EB; ld bc,$F7F7; out(c),a (win3->pg20;
                                                      #   window 3 = $C000, unused — not the code/screen)
            + attrs_and_halt())


def uno_program() -> bytes:
    return (b"\xF3"                                   # di
            b"\x3E\x0B\x01\x3B\xFC\xED\x79"           # ld a,$0B; ld bc,$FC3B; out(c),a
            b"\x3E\x80\x01\x3B\xFD\xED\x79"           # ld a,$80; ld bc,$FD3B; out(c),a (turbo)
            b"\x3E\x02\xD3\xFF"                       # ld a,2; out ($FF),a  (SCLD mode 2)
            + attrs_and_halt())


def dump_run(exe: Path, machine: str, prog: bytes, tmp: Path):
    sna = tmp / f"{machine}.sna"
    dump = tmp / f"{machine}.json"
    build_sna_128k(sna, prog)
    subprocess.run([str(exe), "--sna", str(sna), "--machine", machine,
                    "--rom48128", str(ROM128), "--cycles", "300000",
                    "--dump-out", str(dump), "--no-bmp", "--no-status",
                    "--out", str(tmp / f"{machine}.out.sna")],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return dump


def resume_render(exe: Path, dump: Path, tmp: Path, tag: str) -> Path:
    bmp = tmp / f"{tag}.resume.bmp"
    subprocess.run([str(exe), "--resume", str(dump), "--cycles", "1",
                    "--bmp-out", str(bmp), "--no-dump", "--no-status",
                    "--out", str(tmp / f"{tag}.resume.out.sna")],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return bmp


def check_render(bmp: Path, tag: str) -> bool:
    ok = True
    l0 = bmp_index_at(bmp, BORDER + 4, BORDER + 0)
    l1 = bmp_index_at(bmp, BORDER + 4, BORDER + 1)
    if l0 != IDX_RED_BRIGHT:
        print(f"FAIL: {tag} resumed line0 = {l0}, expected red {IDX_RED_BRIGHT}"); ok = False
    if l1 != IDX_GREEN_BRIGHT:
        print(f"FAIL: {tag} resumed line1 = {l1}, expected green {IDX_GREEN_BRIGHT} "
              f"(8x1 lost -> resume didn't re-arm the machine)"); ok = False
    return ok


def main() -> int:
    exe = find_inspeccy()
    if not ROM128.is_file():
        print(f"SKIP-as-fail: {ROM128} missing"); return 1
    ok = True
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        # ---- ZX-Evo ----
        evo_dump = dump_run(exe, "zxevo", evo_program(), tmp)
        d = json.loads(evo_dump.read_text())
        mem = d.get("memory", {})
        if not mem.get("zxevo"):
            print(f"FAIL: zxevo dump missing 'zxevo' flag"); ok = False
        if str(mem.get("eff7", "")).lower() not in ("$20", "0x20", "32"):
            print(f"FAIL: zxevo dump eff7={mem.get('eff7')}, expected $20"); ok = False
        if int(str(mem.get("evo_win3", -1)).lstrip("$") or -1) != 20:
            print(f"FAIL: zxevo dump evo_win3={mem.get('evo_win3')}, expected 20"); ok = False
        ok = check_render(resume_render(exe, evo_dump, tmp, "evo"), "zxevo") and ok

        # ---- ZX-Uno ----
        uno_dump = dump_run(exe, "zxuno", uno_program(), tmp)
        d = json.loads(uno_dump.read_text())
        mem = d.get("memory", {})
        if not mem.get("zxuno"):
            print(f"FAIL: zxuno dump missing 'zxuno' flag"); ok = False
        if str(mem.get("scld_mode", "")).lower() not in ("$02", "0x02", "2"):
            print(f"FAIL: zxuno dump scld_mode={mem.get('scld_mode')}, expected $02"); ok = False
        if str(mem.get("uno_scandbl", "")).lower() not in ("$80", "0x80", "128"):
            print(f"FAIL: zxuno dump uno_scandbl={mem.get('uno_scandbl')}, expected $80"); ok = False
        ok = check_render(resume_render(exe, uno_dump, tmp, "uno"), "zxuno") and ok

        if ok:
            print("PASS: ZX-Evo + ZX-Uno dump/resume round-trip (state + HWMC/8x1 render)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
