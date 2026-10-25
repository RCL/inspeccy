"""Copper-split tilemap scroll in --video-out recordings (2026-10-05).

A mode-3 copper list writes the tilemap X offset (NR $2F/$30) for display lines
0..191 and resets it to 0 at line 192; the CPU HALTs every frame, then steps
the offset by one pixel and patches the two MOVE values in copper memory
through NR $61 / NR $60. The tilemap is red with one blue column (column 0),
so the column's x on a recorded frame is the offset the beam used there.

Three faults made such a scroll record frozen or flicker:
  * a beam flush could run while the copper lagged (a HALT fast-forward
    catches it up lazily), composing lines from stale registers before the
    copper's MOVEs, which then landed in the past;
  * --dump-out kept only the copper's counters, so --resume ran on with an
    empty, stopped copper;
  * a WAIT whose target sat a few T-states behind the copper's beam (the
    frame origin is re-derived from the interrupt phase, which jitters with a
    busy CPU) was parked for a whole frame instead of matching on its line.

Asserts, at 50 Hz with --video-out --pixel-accurate:
  1. a FIXED list (offset 40, no patching): the column is at x 280 on display
     rows above line 192 and at x 0 below it, in every recorded frame;
  2. the patched list: the column moves exactly one pixel left a frame;
  3. the same after --dump-out at frame 12 and --resume.
The third fault needs a busy CPU's interrupt jitter, which a test program
does not reproduce reliably; test_copper_wait_jitter.cpp replays it directly.
Skips without sjasmplus.
"""
from __future__ import annotations

import shutil
import struct
import subprocess
import sys
from pathlib import Path

from test_support import locate_test_binary

HERE = Path(__file__).resolve().parents[1]
TMP = HERE / "tmp" / "copper_scroll_video"

ASM = """
	device zxspectrumnext
	org $8000
start:
	di
	nextreg $07, 3
	nextreg $43, %00000000		; ula transparent: entry 0 = global transparency
	nextreg $40, 0
	nextreg $41, $e3
	nextreg $14, $e3
	nextreg $43, %00110000		; tilemap palette: 1 red, 2 blue
	nextreg $40, 1
	nextreg $41, $e0
	nextreg $41, $03
	nextreg $52, 11			; tile definitions: tile 0 red, tile 1 blue
	ld hl, $4000
	ld de, $4001
	ld bc, 31
	ld (hl), $11
	ldir
	ld hl, $4020
	ld de, $4021
	ld bc, 31
	ld (hl), $22
	ldir
	ld hl, $5000			; the map: column 0 blue, the rest red
	ld c, 32
.row:
	ld (hl), 1
	inc hl
	ld (hl), 0
	inc hl
	ld b, 39
.col:
	ld (hl), 0
	inc hl
	ld (hl), 0
	inc hl
	djnz .col
	dec c
	jr nz, .row
	nextreg $52, 10
	nextreg $6f, $20
	nextreg $6e, $30
	nextreg $6b, $80
	nextreg $62, 0			; upload the list through nr $60
	nextreg $61, 0
	ld hl, list
	ld b, list_end - list
.up:
	ld a, (hl)
	inc hl
	nextreg $60, a
	djnz .up
	nextreg $62, $c0		; run, restart every frame
	ld hl, $fe00			; im 2, every vector to $fdfd
	ld de, $fe01
	ld bc, 256
	ld (hl), $fd
	ldir
	ld a, $c3
	ld ($fdfd), a
	ld hl, isr
	ld ($fdfe), hl
	ld a, $fe
	ld i, a
	im 2
	ei
loop:
	halt
	if !FIXED
	ld hl, (offset)
	inc hl
	ld de, 320
	or a
	sbc hl, de
	add hl, de
	jr c, .keep
	ld hl, 0
.keep:
	ld (offset), hl
	nextreg $61, 7			; patch the two move values
	ld a, h
	nextreg $60, a
	nextreg $61, 9
	ld a, l
	nextreg $60, a
	endif
	jr loop
isr:
	ei
	reti
offset: dw 0
list:
	db $2f,0, $30,0, $80,0, $2f,0, $30,FIXED*40
	db $80,192, $2f,0, $30,0, $ff,$ff
list_end:
	savenex open NEXNAME, start, $bff0
	savenex core 3, 0, 0
	savenex auto
	savenex close
"""

# Composite rows: display line D is row 32 + D.
ROWS_SCROLLED = (40, 120, 220)  # display lines 8, 88, 188
ROW_BELOW = 240  # display line 208, after the reset at line 192


def read_bmp(path: Path):
    b = path.read_bytes()
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bits = struct.unpack_from("<H", b, 28)[0]
    if bits != 24:
        raise ValueError(f"{path.name}: {bits} bpp, expected 24")
    pitch = (w * 3 + 3) & ~3
    flip = h > 0
    h = abs(h)

    def pixel(x: int, y: int):
        row = h - 1 - y if flip else y
        p = off + row * pitch + x * 3
        return b[p + 2], b[p + 1], b[p]  # r, g, b

    return w, pixel


def blue_x(pixel, width: int, y: int):
    for x in range(width):
        r, g, b = pixel(x, y)
        if b > 128 and r < 64:
            return x
    return None


def assemble(inspeccy_dir: Path, fixed: int) -> Path:
    name = f"scroll_{'fixed' if fixed else 'patched'}.nex"
    asm = TMP / f"scroll_{fixed}.asm"
    asm.write_text(ASM, encoding="utf-8")
    r = subprocess.run(["sjasmplus", f"-DFIXED={fixed}", f'-DNEXNAME="{name}"', asm.name], cwd=TMP,
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"sjasmplus failed\n{r.stdout}\n{r.stderr}")
    return TMP / name


def record(inspeccy: Path, source: list[str], folder: Path, budget: list[str]) -> list[Path]:
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    r = subprocess.run([str(inspeccy), "--machine", "next50", "--no-status", "--no-bmp", "--no-ay", *source,
                        *budget, "--video-out", str(folder / "f%06d.bmp"), "--video-fps", "50",
                        "--pixel-accurate"], capture_output=True, text=True)
    if r.returncode not in (0, 2):
        raise RuntimeError(f"inspeccy exit {r.returncode}\n{r.stdout}\n{r.stderr}")
    return sorted(folder.glob("f*.bmp"))


def column_steps(frames: list[Path]) -> list[int]:
    xs = []
    for f in frames:
        w, pixel = read_bmp(f)
        xs.append(blue_x(pixel, w, 120))
    return [(b - a) % 320 for a, b in zip(xs, xs[1:]) if a is not None and b is not None]


def main() -> int:
    argv = sys.argv[1:]
    inspeccy = Path(argv[argv.index("--inspeccy") + 1]) if "--inspeccy" in argv else locate_test_binary()
    if inspeccy is None or not inspeccy.is_file():
        print("copper_scroll_video: SKIP (no inspeccy binary)")
        return 0
    if shutil.which("sjasmplus") is None:
        print("copper_scroll_video: SKIP (sjasmplus not on PATH)")
        return 0
    TMP.mkdir(parents=True, exist_ok=True)
    failures = []

    # 1. the fixed split, in every frame once the program is up
    nex = assemble(inspeccy.parent, 1)
    frames = record(inspeccy, ["--nex", str(nex)], TMP / "fixed", ["--until-frame", "20"])[12:]
    for f in frames:
        w, pixel = read_bmp(f)
        above = [blue_x(pixel, w, y) for y in ROWS_SCROLLED]
        below = blue_x(pixel, w, ROW_BELOW)
        if above != [280] * len(ROWS_SCROLLED) or below != 0:
            failures.append(f"fixed split, {f.name}: column at {above} above line 192 (want 280), "
                            f"{below} below (want 0)")
            break

    # 2. the patched scroll, one pixel a frame
    nex = assemble(inspeccy.parent, 0)
    steps = column_steps(record(inspeccy, ["--nex", str(nex)], TMP / "patched", ["--until-frame", "30"])[12:])
    if not steps or any(s != 319 for s in steps):
        failures.append(f"patched scroll: steps {steps} (want every step 319 = one pixel left)")

    # 3. the same across --dump-out / --resume
    dump = TMP / "lead.json"
    subprocess.run([str(inspeccy), "--machine", "next50", "--no-status", "--no-bmp", "--no-ay", "--nex", str(nex),
                    "--until-frame", "12", "--dump-out", str(dump)], capture_output=True, text=True)
    steps = column_steps(record(inspeccy, ["--resume", str(dump)], TMP / "resumed", ["--seconds", "0.3"]))
    if not steps or any(s != 319 for s in steps):
        failures.append(f"resumed scroll: steps {steps} (want every step 319; a resume that drops the copper "
                        f"freezes the offset)")

    if failures:
        print("copper_scroll_video: FAIL")
        for f in failures:
            print("  " + f)
        return 1
    print("copper_scroll_video: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
