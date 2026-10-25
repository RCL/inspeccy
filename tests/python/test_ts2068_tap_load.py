#!/usr/bin/env python3
"""TS2068/TC2068: load a generated tape through a cold boot.

Unlike a synthetic snapshot, this path runs the Home ROM initialization and
preserves the RAM-resident bank pager. See test_ts2068_boot_ram.py for the
pager checks.

Tape loading exercises SCLD port readback, $F4 and $FF banking, the EXROM
image, and the EXROM $00FC tape trap. The classic $0556 trap must require
the Home ROM: the same address in the EXROM contains ordinary code.

Exit codes: 0 = pass, 1 = fail, 77 = skipped (no 2068 ROM).
"""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

HERE = Path(__file__).resolve().parents[1]
INSPECCY_DIR = HERE.parent

BIN_CANDIDATES = binary_candidates()
ROM_CANDIDATES = (test_rom("tc2068-0.rom"),)

LOAD_ADDR = 0x8000
MARKER = bytes((0xF3, 0x76))        # di : halt -- distinctive, and it parks the CPU


def find(paths):
    for p in paths:
        if p.is_file():
            return p
    return None


def make_tap() -> bytes:
    """BASIC one-liner `LOAD "" CODE` + `RANDOMIZE USR 32768`, then a CODE block
    of MARKER at $8000. If the tape loads at all, $8000 holds MARKER."""
    def block(payload: bytes) -> bytes:
        chk = 0
        for b in payload:
            chk ^= b
        body = payload + bytes((chk,))
        return struct.pack("<H", len(body)) + body

    # LOAD "" CODE : RANDOMIZE USR VAL "32768"
    line = (bytes((0xEF, 0x22, 0x22, 0x20, 0xAF, 0x3A, 0x20, 0xF9, 0xC0, 0xB0, 0x22))
            + b"32768" + bytes((0x22, 0x0D)))
    prog = bytes((0x00, 0x0A)) + struct.pack("<H", len(line)) + line
    basic_hdr = bytes((0x00, 0x00)) + b"loader    " + struct.pack("<HHH", len(prog), 10, len(prog))

    code = MARKER + bytes(14)
    code_hdr = bytes((0x00, 0x03)) + b"code      " + struct.pack("<HHH", len(code), LOAD_ADDR, 0x8000)

    return (block(basic_hdr) + block(bytes((0xFF,)) + prog)
            + block(code_hdr) + block(bytes((0xFF,)) + code))


def bank_bytes(dump_path, bank):
    """Pull one RAM bank out of a --dump-out JSON as raw bytes.

    The dump stores each bank as a hexdump ("5:0000   80 49 40 ..."), which is
    readable in a diff and cheap to parse back."""
    import json
    import re
    with open(dump_path, encoding="utf-8") as fh:
        text = json.load(fh)["ram"].get(f"bank_{bank}", "")
    out = bytearray()
    for line in text.splitlines():
        m = re.match(r"^\d+:[0-9A-Fa-f]+\s+(.*)$", line)
        if m:
            out += bytes(int(x, 16) for x in m.group(1).split())
    return bytes(out)


def main() -> int:
    exe = find(BIN_CANDIDATES)
    if exe is None:
        print("FAIL: inspeccy binary not found")
        return 1
    rom = find(ROM_CANDIDATES)
    if rom is None:
        print("SKIP: no 2068 Home ROM available (copyrighted, not shipped)")
        return 77

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        tap = td / "t.tap"
        tap.write_bytes(make_tap())
        out = td / "after.json"

        # NOT --out: inspeccy refuses to write a .sna for a Timex machine,
        # because SNA cannot describe the SCLD and the file would load back as
        # a different machine. --dump-out is the sanctioned path and carries a
        # hexdump of every RAM bank.
        cmd = [str(exe), "--tap", str(tap), "--machine", "ts2068",
               "--rom48", str(rom), "--until-frame", "2000",
               "--reset-trap", "off", "--dump-out", str(out),
               "--no-bmp", "--no-status", "--no-ay"]
        subprocess.run(cmd, capture_output=True, text=True)

        if not out.is_file():
            print("FAIL: no dump produced")
            return 1

        ram = bank_bytes(out, 2)          # $8000-$BFFF on the 48K-class map
        got = ram[LOAD_ADDR - 0x8000: LOAD_ADDR - 0x8000 + len(MARKER)]
        if got != MARKER:
            print(f"FAIL: tape never loaded on ts2068 -- ${LOAD_ADDR:04X} = "
                  + " ".join(f"{b:02X}" for b in got)
                  + " (expected " + " ".join(f"{b:02X}" for b in MARKER) + ")")
            return 1

        print(f"PASS: .tap loaded on ts2068 -- marker present at ${LOAD_ADDR:04X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
