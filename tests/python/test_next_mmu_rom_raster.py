"""Next model: MMU $FF selects the ROM, NR $1E/$1F read the live video line,
NEX programs start with the 48K BASIC ROM selected.

A NEX program on next50 (with a ROM image supplied):
  1. reads NR $50 at start (must be $FF = ROM), maps RAM page 40 into slot 0,
     writes a byte there, then writes $FF back to NR $50 and checks that
     ($0000) is again the ROM's first byte ($F3 = DI in every Spectrum BASIC
     ROM) and that the 48K ROM (not the 128K editor) is the one mapped: the
     48K BASIC ROM has $AF (XOR A) at $0001, the 128K editor ROM has $01.
  2. spins on NR $1F until it reads $C0 (line 192, first bottom-border row)
     and then until it reads $00 again - proving the counter moves and wraps.
Each stage that passes bumps the border colour; the final border is 6 (yellow) when
all stages succeed (checked from the BMP corner). Skips without sjasmplus or
without a ROM image (128.rom supplied through INSPECCY_ROM_DIR).
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
	ld e, 0	; stages passed
; ---- stage 1: nr $50 reads $ff at start
	ld bc, $243b
	ld a, $50
	out (c), a
	inc b
	in a, (c)
	cp $ff
	jr nz, done
	inc e
; ---- stage 2: ram page in slot 0 is writable and readable
	nextreg $50, 40
	ld a, $5a
	ld ($0000), a
	ld a, ($0000)
	cp $5a
	jr nz, done
	inc e
; ---- stage 3: $ff maps the rom back (first byte di), 48k rom not editor
	nextreg $50, $ff
	ld a, ($0000)
	cp $f3
	jr nz, done
	ld a, ($0001)
	cp $af
	jr nz, done
	inc e
; ---- stage 4: nr $1f reaches $c0 within a frame or two
	ld bc, $243b
	ld a, $1f
	out (c), a
	inc b
	ld hl, 0
w1:
	in a, (c)
	cp $c0
	jr z, s4ok
	dec hl
	ld a, h
	or l
	jr nz, w1
	jr done
s4ok:
	inc e
; ---- stage 5: ... and wraps back to $00
	ld hl, 0
w2:
	in a, (c)
	or a
	jr z, s5ok
	dec hl
	ld a, h
	or l
	jr nz, w2
	jr done
s5ok:
	inc e
	inc e	; 6 = all good (5 stages + 1, border yellow)
done:
	ld a, e
	out ($fe), a
spin:
	jr spin
	savenex open "mmurom.nex", start, $bfff
	savenex core 3, 0, 0
	savenex cfg 0
	savenex auto
	savenex close
"""

BORDER_RGB = {  # classic palette, bright off
    0: (0, 0, 0), 1: (0, 0, 205), 2: (205, 0, 0), 3: (205, 0, 205),
    4: (0, 205, 0), 5: (0, 205, 205), 6: (205, 205, 0), 7: (205, 205, 205),
}


def locate_inspeccy():
    return locate_test_binary()


def locate_rom() -> Path | None:
    for c in (test_rom("128.rom"),):
        if c.is_file():
            return c
    return None


def corner_pixel(path: Path):
    d = path.read_bytes()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp == 24:
        stride = (w * 3 + 3) & ~3
        y = (abs(h) - 1 - 2) if h > 0 else 2          # row 2 from the top
        base = off + y * stride + 2 * 3
        b, g, r = d[base:base + 3]
        return (r, g, b), (w, h, bpp)
    if bpp == 8:
        pal = [tuple(d[54 + 4 * i: 54 + 4 * i + 3][::-1]) for i in range(256)]
        stride = (w + 3) & ~3
        y = (abs(h) - 1 - 2) if h > 0 else 2
        return pal[d[off + y * stride + 2]], (w, h, bpp)
    raise AssertionError(f"unexpected bpp {bpp}")


def nearest_border(rgb):
    return min(BORDER_RGB, key=lambda k: sum((a - b) ** 2 for a, b in zip(BORDER_RGB[k], rgb)))


def main() -> int:
    argv = sys.argv[1:]
    inspeccy = Path(argv[argv.index("--inspeccy") + 1]) if "--inspeccy" in argv else locate_inspeccy()
    if inspeccy is None or not inspeccy.is_file():
        print("next_mmu_rom_raster: SKIP (no inspeccy binary)")
        return 0
    if shutil.which("sjasmplus") is None:
        print("next_mmu_rom_raster: SKIP (sjasmplus not on PATH)")
        return 0
    rom = locate_rom()
    if rom is None:
        print("next_mmu_rom_raster: SKIP (no 128.rom image)")
        return 0
    TMP.mkdir(exist_ok=True)
    asm = TMP / "mmurom.asm"
    asm.write_text(ASM, encoding="utf-8")
    r = subprocess.run(["sjasmplus", asm.name], cwd=TMP, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"next_mmu_rom_raster: sjasmplus failed\n{r.stdout}\n{r.stderr}")
        return 1
    nex = TMP / "mmurom.nex"
    bmp = TMP / "mmurom.bmp"
    if bmp.exists():
        bmp.unlink()
    p = subprocess.run([str(inspeccy), "--nex", str(nex), "--machine", "next50",
                        "--rom48128", str(rom), "--cycles", "3000000",
                        "--bmp-out", str(bmp), "--no-status", "--no-dump", "--no-ay",
                        "--out", str(TMP / "mmurom.out.sna")],
                       capture_output=True, text=True)
    if not bmp.is_file():
        print(f"next_mmu_rom_raster: no BMP produced\n{p.stdout}\n{p.stderr}")
        return 1
    rgb, info = corner_pixel(bmp)
    if info[0] != 320:
        print(f"next_mmu_rom_raster: SKIP (capture {info} has no border)")
        return 0
    stage = nearest_border(rgb)
    names = {0: "NR $50 did not read $FF at start", 1: "RAM page in slot 0 not writable/readable",
             2: "NR $50=$FF did not map the 48K BASIC ROM back", 3: "NR $1F never reached $C0",
             4: "NR $1F never wrapped to $00"}
    if stage != 6:
        print(f"next_mmu_rom_raster: FAIL at stage {stage + 1}: {names.get(stage, '?')} (border rgb {rgb})")
        return 1
    print("next_mmu_rom_raster: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
