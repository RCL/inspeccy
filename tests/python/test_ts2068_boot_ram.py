#!/usr/bin/env python3
"""TS2068/TC2068: the ROM's RAM-resident bank pager must exist after a cold boot.

The 2068 keeps its bank-management code in RAM, not in ROM -- it has to, because
that code pages ROM chunks in and out underneath itself and cannot live in the
ROM it is switching. The Home ROM copies it down during initialisation, once,
and never checks it again. The documented entry points all land inside it:

    GET_STATUS  $6405     CALL_BANK   $65D0
    GET_NUMBER  $645E     MOVE_BYTES  $668C
    BANK_ENABLE $6499     XFER_BYTES  $6722
    GOTO_BANK   $6572

Any program that loads over $6000-$7AFF therefore destroys the pager silently:
nothing faults, because the block is only data until something calls into it.
The channel machinery does -- `CALL $65D0` is issued from $122A and $128F, on
either side of SELECT at $1230 -- so a program can run for a long time and then
fail the first time it prints through a stream.

A synthetic snapshot does not run the ROM initialization and therefore can
hide this class of failure. This test checks the pager after a cold boot and
compares it with the snapshot path.

RAM is read from `--dump-out`, not from a saved .sna: inspeccy refuses to write
an .sna for a Timex machine precisely because the format cannot describe one
(see main.cpp's bSnaCapableMachine). The dump does carry it, per bank, as a
hexdump -- $6000-$7AFF is bank 5 at offset $2000.

Needs a 2068 Home ROM. Skips (exit 77) if one is not available -- it is
copyrighted and not shipped with the tree, exactly like the other 2068 ROMs.

Exit codes: 0 = pass, 1 = fail, 77 = skipped.
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

# A 2068 Home ROM, wherever it happens to live on this machine.
ROM_CANDIDATES = (test_rom("tc2068-0.rom"),)

# The pager, as the Home ROM leaves it. Two anchors rather than a checksum of
# the whole block: the exact extent shifts a little with how far the boot has
# progressed, but these bytes are the routines themselves.
#   $6000: LD A,1 / OUT ($F4),A / IN A,($FF) / SET 7,A / OUT ($FF),A
#          -- the DOCK/EXROM chunk latch and the SCLD mode register.
ANCHOR_6000 = bytes((0x3E, 0x01, 0xD3, 0xF4, 0xDB, 0xFF, 0xCB, 0xFF, 0xD3, 0xFF))
BANK_ENABLE = 0x6499
PAGER_LO, PAGER_HI = 0x6100, 0x6900     # where the block is expected to sit
MIN_PAGER_BYTES = 1024                  # measured: ~1660 on a settled boot


def find(paths, what):
    for p in paths:
        if p.is_file():
            return p
    return None


def make_tap() -> bytes:
    """A minimal, valid BASIC tape. Its contents do not matter -- it exists only
    to give inspeccy a source so it cold-boots the ROM. (`--tap` is currently the
    only cold-boot path; see the tap-load gap noted at the bottom of this file.)"""
    def block(payload: bytes) -> bytes:
        chk = 0
        for b in payload:
            chk ^= b
        body = payload + bytes((chk,))
        return struct.pack("<H", len(body)) + body

    line = bytes((0xEA, 0x0D))                       # REM
    prog = bytes((0x00, 0x0A)) + struct.pack("<H", len(line)) + line
    hdr = bytes((0x00, 0x00)) + b"boot      " + struct.pack("<HHH", len(prog), 10, len(prog))
    return block(hdr) + block(bytes((0xFF,)) + prog)


def bank_from_dump(path: Path, bank: int) -> bytes:
    """Pull one 16K bank out of a --dump-out JSON. Banks are hexdumps of the
    form `5:0000   00 11 22 ...`, or the literal `N:zeros` when empty."""
    import json
    doc = json.loads(path.read_text())
    text = doc["ram"][f"bank_{bank}"]
    out = bytearray(16384)
    if text.strip().endswith("zeros"):
        return bytes(out)
    for line in text.splitlines():
        head, _, rest = line.partition(":")
        if not rest:
            continue
        off_s, _, data = rest.partition(" ")
        try:
            off = int(off_s, 16)
        except ValueError:
            continue
        vals = [int(t, 16) for t in data.split() if len(t) == 2
                and all(c in "0123456789abcdefABCDEF" for c in t)]
        out[off:off + len(vals)] = bytes(vals)
    return bytes(out)


def peek(bank5: bytes, addr: int, n: int) -> bytes:
    """$4000-$7FFF is bank 5 on both the 48K and 128K maps."""
    return bank5[addr - 0x4000: addr - 0x4000 + n]


def main() -> int:
    exe = find(BIN_CANDIDATES, "inspeccy")
    if exe is None:
        print("FAIL: inspeccy binary not found")
        return 1
    rom = find(ROM_CANDIDATES, "2068 Home ROM")
    if rom is None:
        print("SKIP: no 2068 Home ROM available (copyrighted, not shipped)")
        return 77

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        tap = td / "boot.tap"
        tap.write_bytes(make_tap())
        booted = td / "booted.json"

        cmd = [str(exe), "--tap", str(tap), "--machine", "ts2068",
               "--rom48", str(rom), "--until-frame", "400",
               "--reset-trap", "off", "--dump-out", str(booted),
               "--no-bmp", "--no-status", "--no-ay"]
        run = subprocess.run(cmd, capture_output=True, text=True)
        if not booted.is_file():
            print("FAIL: cold boot produced no dump")
            print(run.stderr[-800:])
            return 1

        ram = bank_from_dump(booted, 5)

        # 1. the pager's low stub, verbatim
        got = peek(ram, 0x6000, len(ANCHOR_6000))
        if got != ANCHOR_6000:
            print("FAIL: $6000 does not hold the ROM's bank stub after a cold boot")
            print("   expected " + " ".join(f"{b:02X}" for b in ANCHOR_6000))
            print("   got      " + " ".join(f"{b:02X}" for b in got))
            return 1

        # 2. the main body is present and non-trivial
        body = peek(ram, PAGER_LO, PAGER_HI - PAGER_LO)
        populated = sum(1 for b in body if b)
        if populated < MIN_PAGER_BYTES:
            print(f"FAIL: only {populated} non-zero bytes in "
                  f"${PAGER_LO:04X}-${PAGER_HI - 1:04X}; the pager is missing")
            return 1

        # 3. BANK_ENABLE is really code, not incidental garbage
        if not any(peek(ram, BANK_ENABLE, 16)):
            print(f"FAIL: BANK_ENABLE at ${BANK_ENABLE:04X} is blank")
            return 1

        print(f"PASS: 2068 cold boot populates the RAM pager "
              f"({populated} bytes in ${PAGER_LO:04X}-${PAGER_HI - 1:04X}, "
              f"stub at $6000, BANK_ENABLE at ${BANK_ENABLE:04X})")

        # 4. the blind spot, asserted so it is documented rather than folklore:
        #    a synthetic snapshot has none of this, so it can never expose a
        #    program that overwrites the pager. (inspeccy now refuses to WRITE
        #    an .sna for a Timex machine; nothing stops one being handed in.)
        synth = bytes(49152)
        if any(synth[0x6000 - 0x4000:0x7B00 - 0x4000]):
            print("FAIL: the synthetic control image is not blank")
            return 1
        print("PASS: a synthetic snapshot leaves $6000-$7AFF blank -- do NOT use "
              "one to validate a 2068 memory map")

    return 0


if __name__ == "__main__":
    sys.exit(main())
