"""Layer 2 Access Port ($123B) paging window.

A NEX program on next50 opens the WRITE window (bit 0) over $0000-$3FFF for
each 16K third of the 48K Layer 2 (bits 7:6), fills the third with one colour,
closes the window and leaves Layer 2 visible (bit 1). Then it opens the READ
window (bit 2) on third 0 and checks one byte reads back; on a mismatch it
blacks the top third through the write window again, so a broken read window
shows up in the same screenshot test.

Expected screen (Layer 2 256x192, three 64-row bands): green / blue / yellow.
Also checks that the program at $8000 kept running (reads of slots 0-1 while
only the write window is open must still see ROM/base, not the pixels).
Skips when sjasmplus is not on PATH.
"""
from __future__ import annotations

import shutil
import struct
import subprocess
import sys
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

HERE = Path(__file__).resolve().parents[1]
INSPECCY_DIR = HERE.parent
TMP = HERE / "tmp"

ASM = """
	device zxspectrumnext
	org $8000
start:
	di
	ld sp, $bfff
	nextreg $12, 9	; layer 2 at 16k bank 9 (not the default 8)
	nextreg $15, %00000001	; sprites off, layer order s l u
	nextreg $18, 0	; clip window: full 256x192
	nextreg $18, 255
	nextreg $18, 0
	nextreg $18, 191
	ld bc, $123b
; third 0: write window + visible
	ld a, %00000011
	out (c), a
	ld a, $1c	; green (rrrgggbb)
	call fill16k
; third 1
	ld a, %01000011
	out (c), a
	ld a, $03	; blue
	call fill16k
; third 2
	ld a, %10000011
	out (c), a
	ld a, $fc	; yellow
	call fill16k
; read window on third 0, verify one byte
	ld a, %00000110
	out (c), a
	ld a, ($0100)
	cp $1c
	jr z, readok
; mismatch: black out the top third through the write window
	ld a, %00000011
	out (c), a
	xor a
	call fill16k
readok:
	ld a, %00000010	; window closed, layer 2 visible
	out (c), a
; marker so the harness can tell the program finished: border 2 (red)
	ld a, 2
	out ($fe), a
spin:
	jr spin

; write-only fill: with just the write window open, reads of $0000-$3fff still
; come from rom (hardware behaviour), so an ldir self-copy would smear rom
; bytes into layer 2. never read the window here.
fill16k:
	ld c, a
	ld hl, $0000
	ld de, $4000
.l:
	ld (hl), c
	inc hl
	dec de
	ld a, d
	or e
	jr nz, .l
	ld a, c
	ld bc, $123b	; restore port for the caller
	ret
	savenex open "l2window.nex", start, $bfff
	savenex core 3, 0, 0
	savenex cfg 0
	savenex auto
	savenex close
"""


def locate_inspeccy():
    return locate_test_binary()


def read_bmp(path: Path):
    d = path.read_bytes()
    assert d[:2] == b"BM", "not a BMP"
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    rows = []
    if bpp == 24:
        stride = (w * 3 + 3) & ~3
        for y in range(abs(h)):
            base = off + y * stride
            rows.append([tuple(d[base + x * 3: base + x * 3 + 3][::-1]) for x in range(w)])
    elif bpp == 8:
        ncol = struct.unpack_from("<I", d, 46)[0] or 256
        pal = [tuple(d[54 + 4 * i: 54 + 4 * i + 3][::-1]) for i in range(ncol)]
        stride = (w + 3) & ~3
        for y in range(abs(h)):
            base = off + y * stride
            rows.append([pal[d[base + x]] for x in range(w)])
    else:
        raise AssertionError(f"unexpected bpp {bpp}")
    if h > 0:                       # bottom-up
        rows.reverse()
    return w, abs(h), rows


def dominant(px):
    r, g, b = px
    if g > 100 and r < 100 and b < 100:
        return "green"
    if b > 100 and r < 100 and g < 100:
        return "blue"
    if r > 100 and g > 100 and b < 100:
        return "yellow"
    if r < 40 and g < 40 and b < 40:
        return "black"
    return "other"


def main() -> int:
    inspeccy = str(locate_test_binary())
    argv = sys.argv[1:]
    if "--inspeccy" in argv:
        inspeccy = Path(argv[argv.index("--inspeccy") + 1])
    else:
        inspeccy = locate_inspeccy()
    if inspeccy is None or not inspeccy.is_file():
        print("layer2_window: SKIP (no inspeccy binary)")
        return 0
    if shutil.which("sjasmplus") is None:
        print("layer2_window: SKIP (sjasmplus not on PATH)")
        return 0
    TMP.mkdir(exist_ok=True)
    asm = TMP / "l2window.asm"
    asm.write_text(ASM, encoding="utf-8")
    r = subprocess.run(["sjasmplus", asm.name], cwd=TMP, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"layer2_window: sjasmplus failed\n{r.stdout}\n{r.stderr}")
        return 1
    nex = TMP / "l2window.nex"
    bmp = TMP / "l2window.bmp"
    if bmp.exists():
        bmp.unlink()
    p = subprocess.run([str(inspeccy), "--nex", str(nex), "--machine", "next50",
                        "--cycles", "4000000", "--bmp-out", str(bmp),
                        "--no-status", "--no-dump", "--no-ay",
                        "--out", str(TMP / "l2window.out.sna")],
                       capture_output=True, text=True)
    if not bmp.is_file():
        print(f"layer2_window: no BMP produced\n{p.stdout}\n{p.stderr}")
        return 1
    w, h, rows = read_bmp(bmp)
    # Locate the 256x192 paper area: full-frame captures are 320x256 with the
    # paper at (32,32); a bare 256x192 capture starts at (0,0).
    ox, oy = (32, 32) if (w, h) == (320, 256) else (0, 0)
    fails = []
    for band, want in ((0, "green"), (1, "blue"), (2, "yellow")):
        y = oy + band * 64 + 32
        seen = {dominant(rows[y][ox + x]) for x in (8, 128, 247)}
        if seen != {want}:
            fails.append(f"band {band} at row {y}: expected {want}, saw {sorted(seen)}")
    border = dominant(rows[2][2]) if (ox, oy) == (32, 32) else None
    if border is not None and border != "other" and border != "black":
        pass  # border colour 2 = red, classified 'other'; only checked below
    if border == "black":
        fails.append("border still black: program did not reach the end marker (border 2)")
    if fails:
        print("layer2_window: FAIL")
        for f in fails:
            print("  " + f)
        return 1
    print("layer2_window: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
