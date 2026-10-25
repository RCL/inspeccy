#!/usr/bin/env python3
"""Timex SCLD extended-colour (8x1) rendering regression test.

Builds a minimal 48K .sna whose code:
  * sets a blue border,
  * fills the pixel area $4000-$57FF with $FF (every pixel = ink),
  * fills the CLASSIC attribute area $5800-$5AFF with $38 (white paper /
    black ink)  -- a decoy the SCLD 8x1 renderer must NOT read,
  * fills the SCLD 8x1 attribute area $6000-$77FF with $42 (bright red ink),
  * selects SCLD extended-colour mode:  out ($FF), 2,
  * di : halt.

Then runs it two ways and asserts the SCLD mode diverges from classic ULA:
  * --machine tc2048 : renders every pixel BRIGHT RED (ink from $6000). The
    $5800 decoy is ignored, proving the 8x1 attribute source is $6000+addr.
  * --machine 48k    : renders every pixel BLACK (ink from $5800 = $38), with
    NO bright red -- the classic path never touches $6000.

Guards the Timex SCLD renderer + port $FF decode added 2026-07.

Exit codes: 0 = pass, 1 = fail.
"""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

HERE         = Path(__file__).resolve().parents[1]
INSPECCY_DIR = HERE.parent
BIN_CANDIDATES = binary_candidates()
ROM48 = test_rom("48.rom")

# No ROM images ship with the tree; skip (exit 77) when absent.
EXIT_HALT_DELIBERATE = 6


def find_inspeccy():
    return locate_test_binary()


# ~850k-cycle busy wait (ld bc,$8000 / dec bc : ld a,b : or c : jr nz). Holds
# the finished screen for ~12 frames so --video-out emits it: video frames are
# only written DURING the run, and an immediate di:halt would leave the last
# emitted frame predating the final out ($FF).
HOLD = b"\x01\x00\x80" + b"\x0B\x78\xB1\x20\xFB"


def build_program() -> bytes:
    code = bytearray()
    code += b"\xF3"                          # di
    code += b"\x3E\x01\xD3\xFE"              # ld a,1 : out ($FE),a   -> blue border
    # fill pixels $4000-$57FF (6144 bytes) with $FF (all ink)
    code += b"\x21\x00\x40"                  # ld hl,$4000
    code += b"\x11\x01\x40"                  # ld de,$4001
    code += b"\x01\xFF\x17"                  # ld bc,$17FF (6143)
    code += b"\x36\xFF"                      # ld (hl),$FF
    code += b"\xED\xB0"                      # ldir
    # decoy classic attrs $5800-$5AFF (768) = $38 (white paper / black ink)
    code += b"\x21\x00\x58"                  # ld hl,$5800
    code += b"\x11\x01\x58"                  # ld de,$5801
    code += b"\x01\xFF\x02"                  # ld bc,$02FF (767)
    code += b"\x36\x38"                      # ld (hl),$38
    code += b"\xED\xB0"                      # ldir
    # SCLD 8x1 attrs $6000-$77FF (6144) = $42 (bright red ink)
    code += b"\x21\x00\x60"                  # ld hl,$6000
    code += b"\x11\x01\x60"                  # ld de,$6001
    code += b"\x01\xFF\x17"                  # ld bc,$17FF (6143)
    code += b"\x36\x42"                      # ld (hl),$42
    code += b"\xED\xB0"                      # ldir
    code += b"\x3E\x02\xD3\xFF"              # ld a,2 : out ($FF),a   -> SCLD 8x1 mode
    code += HOLD + b"\xF3\x76"               # hold the screen, then di : halt
    return bytes(code)


def build_program_screen2() -> bytes:
    """Standard 8x8 mode with screen-2 select (out ($FF),1): render must read
    pixels from $6000 and attrs from $7800, ignoring the $4000/$5800 decoy."""
    code = bytearray()
    code += b"\xF3"                          # di
    code += b"\x3E\x01\xD3\xFE"              # ld a,1 : out ($FE),a   -> blue border
    # decoy screen-1 pixels $4000-$57FF = $00 (all paper)
    code += b"\x21\x00\x40\x11\x01\x40\x01\xFF\x17\x36\x00\xED\xB0"
    # decoy screen-1 attrs $5800-$5AFF = $07 (white paper, no ink shown as $00 pix)
    code += b"\x21\x00\x58\x11\x01\x58\x01\xFF\x02\x36\x07\xED\xB0"
    # screen-2 pixels $6000-$77FF = $FF (all ink)
    code += b"\x21\x00\x60\x11\x01\x60\x01\xFF\x17\x36\xFF\xED\xB0"
    # screen-2 attrs $7800-$7AFF = $44 (bright green ink)
    code += b"\x21\x00\x78\x11\x01\x78\x01\xFF\x02\x36\x44\xED\xB0"
    code += b"\x3E\x01\xD3\xFF"              # ld a,1 : out ($FF),a   -> screen-2 select
    code += HOLD + b"\xF3\x76"               # hold the screen, then di : halt
    return bytes(code)


def build_program_hires() -> bytes:
    """512x192 hi-res mode (out ($FF),6): bit2=hi-res, colour bits 3-5 = 0 ->
    paper black / ink white. Both display files filled $FF -> all-white paper."""
    code = bytearray()
    code += b"\xF3"                          # di
    code += b"\x3E\x02\xD3\xFE"              # ld a,2 : out ($FE),a   -> red border
    # file 1 $4000-$57FF = $FF
    code += b"\x21\x00\x40\x11\x01\x40\x01\xFF\x17\x36\xFF\xED\xB0"
    # file 2 $6000-$77FF = $FF
    code += b"\x21\x00\x60\x11\x01\x60\x01\xFF\x17\x36\xFF\xED\xB0"
    code += b"\x3E\x06\xD3\xFF"              # ld a,6 : out ($FF),a   -> hi-res mode
    code += HOLD + b"\xF3\x76"               # hold the screen, then di : halt
    return bytes(code)


def build_sna_48k(path: Path, prog: bytes = None) -> None:
    """48K .sna: 27-byte header + banks 5,2,0 ($4000-$FFFF). PC is popped
    from the stack, so we plant the $8000 entry address at SP."""
    header = bytearray(27)
    header[25] = 0x01                        # IM 1
    entry = 0x8000
    sp = 0xFF00
    header[23:25] = struct.pack("<H", sp)    # SP
    ram = bytearray(0xC000)                   # $4000..$FFFF contiguous
    if prog is None:
        prog = build_program()
    ram[entry - 0x4000 : entry - 0x4000 + len(prog)] = prog
    ram[sp - 0x4000 : sp - 0x4000 + 2] = struct.pack("<H", entry)  # PC on stack
    path.write_bytes(bytes(header) + bytes(ram))


def read_bmp(path: Path):
    """Returns (width, height, [ (r,g,b), ... ]) for a 4bpp palettized BMP."""
    d = path.read_bytes()
    assert d[:2] == b"BM", "not a BMP"
    off = struct.unpack_from("<I", d, 10)[0]
    hdr_size = struct.unpack_from("<I", d, 14)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert bpp == 4, f"expected 4bpp Timex/ULA BMP, got {bpp}"
    pal_off = 14 + hdr_size
    pal = [tuple(d[pal_off + i*4 : pal_off + i*4 + 3][::-1])   # BGRx -> RGB
           for i in range(16)]
    row = ((w + 1) // 2 + 3) & ~3
    pix = []
    for y in range(abs(h)):
        base = off + y * row
        for x in range(w):
            byte = d[base + x // 2]
            idx = (byte >> 4) if (x & 1) == 0 else (byte & 0x0F)
            pix.append(pal[idx])
    return w, abs(h), pix


def read_bmp24(path: Path):
    """Returns (width, height, [ (r,g,b), ... ]) for a 24bpp BMP (the composite
    encoder's format -- no palette)."""
    d = path.read_bytes()
    assert d[:2] == b"BM", "not a BMP"
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    assert bpp == 24, f"expected 24bpp composite BMP, got {bpp}"
    row = (w * 3 + 3) & ~3
    pix = []
    for y in range(abs(h)):
        base = off + y * row
        for x in range(w):
            b, g, r = d[base + x*3 : base + x*3 + 3]
            pix.append((r, g, b))
    return w, abs(h), pix


def classify(pix):
    red = blue = green = white = black = 0
    for (r, g, b) in pix:
        if r > 200 and g < 60 and b < 60:
            red += 1
        elif g > 200 and r < 60 and b < 60:
            green += 1
        elif b > 150 and r < 80 and g < 80:
            blue += 1
        elif r > 190 and g > 190 and b > 190:
            white += 1
        elif r < 30 and g < 30 and b < 30:
            black += 1
    return dict(red=red, blue=blue, green=green, white=white, black=black)


def count_colours(bmp: Path):
    _, _, pix = read_bmp(bmp)
    return classify(pix)


def count_colours24(bmp: Path):
    _, _, pix = read_bmp24(bmp)
    return classify(pix)


def run_video(exe: Path, sna: Path, machine: str, outdir: Path, tmp: Path) -> Path:
    """Runs with --video-out and returns the LAST emitted composite frame (the
    settled screen), or None on failure."""
    outdir.mkdir(exist_ok=True)
    r = subprocess.run(
        [str(exe), "--sna", str(sna), "--machine", machine,
         "--rom48", str(ROM48), "--cycles", "2000000",
         "--video-out", str(outdir / "f%04d.bmp"),
         "--no-bmp", "--no-dump", "--no-status",
         "--out", str(tmp / f"{machine}.vout.sna")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != EXIT_HALT_DELIBERATE:
        print(f"FAIL[video {machine}]: expected exit {EXIT_HALT_DELIBERATE}, got {r.returncode}")
        return None
    frames = sorted(outdir.glob("f*.bmp"))
    if not frames:
        print(f"FAIL[video {machine}]: no video frames emitted")
        return None
    return frames[-1]


def run(exe: Path, sna: Path, machine: str, bmp: Path, tmp: Path) -> bool:
    r = subprocess.run(
        [str(exe), "--sna", str(sna), "--machine", machine,
         "--rom48", str(ROM48), "--cycles", "2000000",
         "--bmp-out", str(bmp), "--no-dump", "--no-status",
         "--out", str(tmp / f"{machine}.out.sna")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != EXIT_HALT_DELIBERATE:
        print(f"FAIL[{machine}]: expected exit {EXIT_HALT_DELIBERATE}, got {r.returncode}")
        return False
    return True


def main() -> int:
    exe = find_inspeccy()
    if not ROM48.is_file():
        print(f"SKIP-as-fail: {ROM48} missing")
        return 1
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        ok = True

        # --- Case 1: SCLD 8x1 extended colour vs classic ULA -----------------
        sna = tmp / "timex_8x1.sna"
        build_sna_48k(sna, build_program())
        tbmp = tmp / "tc2048_8x1.bmp"
        if not run(exe, sna, "tc2048", tbmp, tmp):
            return 1
        c = count_colours(tbmp)
        if c["red"] < 45_000:      # paper 256x192 = 49152 px
            print(f"FAIL: 8x1 expected bright-red paper, got {c['red']} px"); ok = False
        if c["blue"] < 10_000:
            print(f"FAIL: 8x1 expected blue border, got {c['blue']} px"); ok = False
        if c["white"] > 100:
            print(f"FAIL: 8x1 read the $5800 decoy (white {c['white']} px) - "
                  f"attribute source is wrong"); ok = False
        # same SNA under classic 48K: black paper (ink from $5800=$38), no red.
        cbmp = tmp / "c48_8x1.bmp"
        if not run(exe, sna, "48k", cbmp, tmp):
            return 1
        cc = count_colours(cbmp)
        if cc["red"] > 100:
            print(f"FAIL: classic 48k leaked red ({cc['red']} px)"); ok = False
        if cc["black"] < 45_000:
            print(f"FAIL: classic 48k expected black paper, got {cc['black']} px"); ok = False

        # --- Case 2: standard 8x8 screen-2 select (out ($FF),1) --------------
        sna2 = tmp / "timex_scr2.sna"
        build_sna_48k(sna2, build_program_screen2())
        s2 = tmp / "tc2048_scr2.bmp"
        if not run(exe, sna2, "tc2048", s2, tmp):
            return 1
        c2 = count_colours(s2)
        if c2["green"] < 45_000:
            print(f"FAIL: screen-2 expected bright-green paper from $6000/$7800, "
                  f"got {c2['green']} px"); ok = False
        if c2["white"] > 100:
            print(f"FAIL: screen-2 leaked the $4000/$5800 decoy (white {c2['white']} px)")
            ok = False

        # --- Case 3: 512x192 hi-res (out ($FF),6) ----------------------------
        sna3 = tmp / "timex_hires.sna"
        build_sna_48k(sna3, build_program_hires())
        h3 = tmp / "tc2048_hires.bmp"
        if not run(exe, sna3, "tc2048", h3, tmp):
            return 1
        w, hh, _ = read_bmp(h3)
        if w != 608:
            print(f"FAIL: hi-res canvas expected 608 wide (512+border), got {w}"); ok = False
        c3 = count_colours(h3)
        if c3["white"] < 90_000:   # paper 512x192 = 98304 px
            print(f"FAIL: hi-res expected all-white paper, got {c3['white']} px"); ok = False

        # --- Case 4: dump + resume preserves SCLD state ----------------------
        # Re-run 8x1 producing a dump, then resume from it and re-render; the
        # SCLD mode ($FF=2) must survive the dump round-trip.
        dsna = tmp / "timex_dump.sna"
        build_sna_48k(dsna, build_program())
        r = subprocess.run(
            [str(exe), "--sna", str(dsna), "--machine", "tc2048",
             "--rom48", str(ROM48), "--cycles", "2000000",
             "--bmp-out", str(tmp / "dump_run.bmp"), "--out", str(tmp / "d.out"),
             "--no-status"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dump_json = tmp / "d.out.inspec.dump.json"
        if r.returncode != EXIT_HALT_DELIBERATE or not dump_json.is_file():
            print(f"FAIL: dump run exit {r.returncode} / dump present={dump_json.is_file()}")
            ok = False
        else:
            rbmp = tmp / "resumed.bmp"
            rr = subprocess.run(
                [str(exe), "--resume", str(dump_json), "--rom48", str(ROM48),
                 "--cycles", "100000", "--bmp-out", str(rbmp),
                 "--no-dump", "--no-status"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if not rbmp.is_file():
                print(f"FAIL: resume produced no BMP (exit {rr.returncode})"); ok = False
            else:
                cr = count_colours(rbmp)
                if cr["red"] < 45_000 or cr["white"] > 100:
                    print(f"FAIL: resume lost SCLD state (red={cr['red']}, "
                          f"white={cr['white']})"); ok = False

        # --- Case 5: the --video-out path honours SCLD -----------------------
        # Same programs through the per-frame video encoder; the last frame is
        # the settled screen. Guards two things: that video of a Timex-mode
        # program does not show the classic $4000/$5800 view, and (since
        # 2026-08-27) that video and stills go through the SAME machine-aware
        # dispatcher. Video used to call WriteCompositeBmp directly, which
        # rendered every machine on the Next's 320x256 RGB888 canvas -- so a
        # Timex video frame had a 32-pixel border where the same machine's
        # --bmp-out had 48, and anything measured off video by cropping at the
        # stills' border was 16 pixels out. Hence count_colours (4bpp, as the
        # stills use) rather than count_colours24 here.
        v1 = run_video(exe, sna, "tc2048", tmp / "v8x1", tmp)
        if v1 is None:
            return 1
        cv = count_colours(v1)
        if cv["red"] < 45_000:
            print(f"FAIL: composite 8x1 expected bright-red paper, got {cv['red']} px"); ok = False
        if cv["white"] > 100:
            print(f"FAIL: composite 8x1 read the $5800 decoy ({cv['white']} white px)"); ok = False
        # classic 48k control through the same path: no red may leak.
        v1c = run_video(exe, sna, "48k", tmp / "v8x1c", tmp)
        if v1c is None:
            return 1
        cvc = count_colours(v1c)
        if cvc["red"] > 100:
            print(f"FAIL: composite classic 48k leaked red ({cvc['red']} px)"); ok = False
        v2 = run_video(exe, sna2, "tc2048", tmp / "vscr2", tmp)
        if v2 is None:
            return 1
        c2v = count_colours(v2)
        if c2v["green"] < 45_000:
            print(f"FAIL: composite screen-2 expected green paper, got {c2v['green']} px"); ok = False
        v3 = run_video(exe, sna3, "tc2048", tmp / "vhires", tmp)
        if v3 is None:
            return 1
        c3v = count_colours(v3)
        if c3v["white"] < 45_000:   # 512 halved into the 256-wide slot
            print(f"FAIL: composite hi-res expected white paper, got {c3v['white']} px"); ok = False

        if ok:
            print("PASS: SCLD 8x1 (red paper from $6000), classic-48k divergence "
                  "(black), screen-2 select (green from $6000/$7800), 512-wide "
                  "hi-res (white), dump/resume SCLD round-trip, and the composite "
                  "video path for all three modes correct")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
