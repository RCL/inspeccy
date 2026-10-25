"""+2A/+3 paging: ROM selection via $7FFD bit4 + $1FFD bit2, and Marta's $0FFD
floating-bus|1 quirk.

A CODE block (assembled here, wrapped in a minimal autostart BASIC loader into a
.tap built in Python) runs on --machine +3 and, with interrupts off:
  * selects ROM 3 (48 BASIC) and checks ($0001) == $AF (XOR A, the 48 ROM's 2nd byte)
  * selects ROM 1 (128 syntax)  and checks ($0000) == $53 ('S')
  * selects ROM 0 (128 editor)  and checks ($0001) == $01
  * reads port $0FFD and checks bit 0 is set (the +3 floating-bus|1 quirk)
It counts the passing checks into the border colour and HALTs; the run's status
reports vram.border_colour, which must be 4 (all four checks passed).

Skips without sjasmplus or the +3 ROM image (INSPECCY_ROM_DIR/plus3.rom).
"""
from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

HERE = Path(__file__).resolve().parents[1]
INSPECCY_DIR = HERE.parent
TMP = HERE / "tmp"

CODE_ASM = """
	device zxspectrum128
	org $8000
start:
	di
	ld sp, $bfff
	ld d, 0	; pass count
; --- rom 3 = 48 basic: $7ffd bit4=1 (low rom bit), $1ffd bit2=1 (high) ---
	ld bc, $7ffd
	ld a, $10
	out (c), a
	ld bc, $1ffd
	ld a, $04
	out (c), a
	ld a, ($0001)
	cp $af
	jr nz, s1
	inc d
s1:
; --- rom 1 = 128 syntax: $7ffd bit4=1, $1ffd bit2=0 ---
	ld bc, $1ffd
	xor a
	out (c), a
	ld a, ($0000)
	cp $53
	jr nz, s2
	inc d
s2:
; --- rom 0 = 128 editor: $7ffd bit4=0, $1ffd bit2=0 ---
	ld bc, $7ffd
	xor a
	out (c), a
	ld a, ($0001)
	cp $01
	jr nz, s3
	inc d
s3:
; --- $0ffd: bit 0 forced to 1 (marta's quirk) ---
	ld bc, $0ffd
	in a, (c)
	and 1
	jr z, s4
	inc d
s4:
	ld a, d
	out ($fe), a	; border = number of checks that passed
	halt
	savebin "code.bin", start, $ - start
"""


def _tap_block(flag: int, data: bytes) -> bytes:
    body = bytes([flag]) + data
    chk = 0
    for b in body:
        chk ^= b
    body += bytes([chk])
    return struct.pack("<H", len(body)) + body


def build_tap(code: bytes, out: Path) -> None:
    # Minimal autostart BASIC: line 10  RANDOMIZE USR 32768 : the loader types
    # LOAD "" which runs this, and line 10 loads the CODE block (headed, to its
    # own address) then calls it. We keep BASIC to two statements:
    #   10 LOAD ""CODE : RANDOMIZE USR 32768
    LOAD, CODE_TOK, RANDOMIZE, USR = 0xEF, 0xAF, 0xF9, 0xC0
    num = b"\x0e\x00\x00\x00\x80\x00"           # 5-byte float form of 32768
    line = (bytes([LOAD, 0x22, 0x22, CODE_TOK, 0x3A, RANDOMIZE, USR])
            + b"32768" + num + bytes([0x0D]))
    prog = struct.pack(">H", 10) + struct.pack("<H", len(line)) + line   # line no big-endian
    # BASIC header: type 0, name, len, autostart line 10, prog len.
    name = b"tester    "[:10]
    bhdr = bytes([0]) + name + struct.pack("<H", len(prog)) + struct.pack("<H", 10) + struct.pack("<H", len(prog))
    # CODE header: type 3, name, len, start addr $8000, 0.
    chdr = bytes([3]) + b"code      "[:10] + struct.pack("<H", len(code)) + struct.pack("<H", 0x8000) + struct.pack("<H", 0)
    tap = (_tap_block(0x00, bhdr) + _tap_block(0xFF, prog)
           + _tap_block(0x00, chdr) + _tap_block(0xFF, code))
    out.write_bytes(tap)


def locate_inspeccy():
    return locate_test_binary()


def locate_rom() -> Path | None:
    p = test_rom("plus3.rom")
    return p if p.is_file() else None


def main() -> int:
    argv = sys.argv[1:]
    inspeccy = Path(argv[argv.index("--inspeccy") + 1]) if "--inspeccy" in argv else locate_inspeccy()
    if inspeccy is None or not inspeccy.is_file():
        print("plus3_paging: SKIP (no inspeccy binary)")
        return 77
    if shutil.which("sjasmplus") is None:
        print("plus3_paging: SKIP (sjasmplus not on PATH)")
        return 77
    rom = locate_rom()
    if rom is None:
        print("plus3_paging: SKIP (no plus3.rom image)")
        return 77
    TMP.mkdir(exist_ok=True)
    asm = TMP / "p3code.asm"
    asm.write_text(CODE_ASM, encoding="utf-8")
    r = subprocess.run(["sjasmplus", asm.name], cwd=TMP, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"plus3_paging: sjasmplus failed\n{r.stdout}\n{r.stderr}")
        return 1
    code = (TMP / "code.bin").read_bytes()
    tap = TMP / "p3test.tap"
    build_tap(code, tap)
    status = TMP / "p3test.json"
    # Contention is disabled here to isolate paging from boot-menu timing.
    subprocess.run([str(inspeccy), "--tap", str(tap), "--machine", "+3",
                    "--rom-plus3", str(rom), "--seconds", "6", "--no-ay",
                    "--contention", "off",
                    "--no-bmp", "--no-dump", "--status-out", str(status),
                    "--out", str(TMP / "p3test.out.sna")],
                   capture_output=True, text=True)
    if not status.is_file():
        print("plus3_paging: FAIL (no status output)")
        return 1
    j = json.loads(status.read_text(encoding="utf-8"))
    border = j.get("vram", {}).get("border_colour")
    if border != 4:
        print("plus3_paging: FAIL")
        print(f"  expected border 4 (all four ROM-select / $0FFD checks passed), got {border}")
        return 1
    print("plus3_paging: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
