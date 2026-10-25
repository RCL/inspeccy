"""Check frame replay against a static reference and the eager pixel renderer.

All fixtures run without a ROM. The Layer 2 check verifies all 81920 pixels.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from test_support import locate_test_binary

HERE = Path(__file__).resolve().parents[1]
L2_320_ASM = """
	device zxspectrumnext
	org $8000
start:
	di
	ld sp, $bfff
	nextreg $43, $10
	nextreg $40, 1
	nextreg $44, $1c
	nextreg $44, 0
	nextreg $70, $10
	nextreg $12, 9
	nextreg $69, $80
; keep the page counter outside bc, which ldir consumes.
	ld a, 18
fillpage:
	nextreg $52, a
	push af
	ld hl, $4000
	ld (hl), 1
	ld de, $4001
	ld bc, 8191
	ldir
	pop af
	inc a
	cp 28
	jr nz, fillpage
	nextreg $52, 4
spin:
	jr spin
	savenex open "l2_320_fill.nex", start, $bfff
	savenex core 3, 0, 0
	savenex cfg 0
	savenex auto
	savenex close
"""


def assemble(source, directory, output, raw=False):
    args = ["sjasmplus", f"-i{HERE / 'data' / 'copper'}", f"-i{HERE / 'asm'}"]
    if raw:
        args.append(f"--raw={output}")
    result = subprocess.run(args + [source.as_posix()], cwd=directory,
                            capture_output=True, text=True)
    diagnostics = result.stdout + result.stderr
    if result.returncode or "warning:" in diagnostics.lower() or not output.is_file():
        raise RuntimeError(f"assembly failed for {source.name}:\n{diagnostics}")


def render(binary, args, output):
    result = subprocess.run([str(binary), *args, "--no-ay", "--no-dump",
                             "--no-status", "--bmp-out", str(output)],
                            capture_output=True, text=True)
    if result.returncode not in (0, 5) or not output.is_file():
        raise RuntimeError(f"render failed ({result.returncode}): {result.stderr}")
    return output.read_bytes()


def check_green(image):
    offset = struct.unpack_from("<I", image, 10)[0]
    width, height = struct.unpack_from("<ii", image, 18)
    depth = struct.unpack_from("<H", image, 28)[0]
    assert (width, height, depth) == (320, 256, 24), "unexpected BMP geometry"
    assert image[offset:] == bytes((0, 255, 0)) * (320 * 256), "incomplete Layer 2 fill"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inspeccy", type=Path)
    options = parser.parse_args()
    binary = (options.inspeccy or locate_test_binary()).resolve()
    if shutil.which("sjasmplus") is None:
        print("bmp_frame_exact: SKIP (sjasmplus not on PATH)")
        return 77
    with tempfile.TemporaryDirectory(prefix="inspeccy-frame-") as temporary:
        directory = Path(temporary)
        copper, software = (directory / f"{name}.dot" for name in ("page_copper", "page_soft"))
        for output in (copper, software):
            assemble(HERE / "data" / "copper" / f"{output.stem}.asm", directory, output, raw=True)
        common = ["--machine", "next50", "--seconds", "1"]
        expected = render(binary, ["--dot", str(software), *common, "--bmp-exact", "off"], directory / "soft.bmp")
        actual = render(binary, ["--dot", str(copper), *common], directory / "copper.bmp")
        snapshot = render(binary, ["--dot", str(copper), *common, "--bmp-exact", "off"], directory / "snapshot.bmp")
        assert actual == expected, "copper replay differs from software reference"
        assert snapshot != expected, "snapshot no longer distinguishes the copper page flip"

        # Stop between frame boundaries, after initialization and before return.
        midframe = ["--dot", str(copper), "--machine", "next50", "--cycles", "1500123"]
        replay = render(binary, midframe, directory / "midframe.bmp")
        eager = render(binary, [*midframe, "--pixel-accurate"], directory / "eager.bmp")
        assert replay == eager, "mid-frame replay differs from eager rendering"
        assert replay == expected, "mid-frame check ran before copper initialization"

        source = directory / "l2_320_fill.asm"
        source.write_text(L2_320_ASM, encoding="ascii")
        nex = directory / "l2_320_fill.nex"
        assemble(source, directory, nex)
        args = ["--nex", str(nex), "--machine", "next50", "--until-frame", "40"]
        replay = render(binary, args, directory / "320.bmp")
        snapshot = render(binary, [*args, "--bmp-exact", "off"], directory / "320_snapshot.bmp")
        check_green(snapshot)
        assert replay == snapshot, "completed 320x256 frame differs from snapshot"
    print("bmp_frame_exact: PASS (reference, mid-frame replay, all Layer 2 pixels)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
