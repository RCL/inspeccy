"""Regression checks for the wav-compare/ssim-compare companion binaries.

Every fixture is written here with the standard library, so this needs no
Pillow, no numpy and no assembler -- only the two built companions.
"""
from __future__ import annotations

import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2]


def companion(name):
    """The built binary, wherever this tree happens to put it."""
    candidates = [SOURCE / "build/companions" / name / name,
                  SOURCE / "build/companions" / name / (name + ".exe"),
                  SOURCE / "bin" / name, SOURCE / "bin" / (name + ".exe")]
    executable = os.environ.get("INSPECCY")
    if executable:
        for base in Path(executable).resolve().parents[:3]:
            for config in ("", "Release", "Debug"):
                candidates.extend(base / "companions" / name / config / filename
                                  for filename in (name, name + ".exe"))
    on_path = shutil.which(name)
    if on_path:
        candidates.append(Path(on_path))
    return next((c for c in candidates if c.is_file()), None)


def grey_bmp(path, width, height, pixels):
    """An 8-bit greyscale BMP, bottom-up, with an identity palette."""
    stride = (width + 3) & ~3
    rows = b"".join(bytes(pixels[y * width:(y + 1) * width]) + bytes(stride - width)
                    for y in range(height - 1, -1, -1))
    palette = b"".join(struct.pack("BBBB", i, i, i, 0) for i in range(256))
    start = 14 + 40 + len(palette)
    path.write_bytes(
        b"BM" + struct.pack("<IHHI", start + len(rows), 0, 0, start)
        + struct.pack("<IiiHHIIiiII", 40, width, height, 1, 8, 0, len(rows),
                      2835, 2835, 256, 0)
        + palette + rows)


def main():
    ssim = companion("ssim-compare")
    wav_tool = companion("wav-compare")
    if len(sys.argv) > 2:
        ssim, wav_tool = Path(sys.argv[1]), Path(sys.argv[2])
    if ssim is None or wav_tool is None:
        print("SKIP: comparison companions are not built")
        return 77

    with tempfile.TemporaryDirectory(prefix="inspeccy_compare_") as temporary:
        work = Path(temporary)

        def run(tool, args, expected):
            result = subprocess.run([str(tool), *map(str, args)],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
            assert "Traceback" not in result.stderr
            return result.stdout

        def wav(name, frequency):
            path = work / name
            samples = [round(12000 * math.sin(2 * math.pi * frequency * i / 11025))
                       if frequency else 0 for i in range(4096)]
            with wave.open(str(path), "wb") as output:
                output.setparams((1, 2, 11025, 0, "NONE", "not compressed"))
                output.writeframes(struct.pack("<" + "h" * len(samples), *samples))
            return path

        tone = wav("tone.wav", 11025 / 16)
        other = wav("other.wav", 11025 / 8)
        silence = wav("silence.wav", 0)
        run(wav_tool, [tone, tone, "--max-shift-s", 0], 0)
        run(wav_tool, [silence, silence], 0)
        run(wav_tool, [tone, other], 1)
        run(wav_tool, [tone, silence], 1)
        for option, value in (("--skip-s", "nan"), ("--max-shift-s", "-1"),
                              ("--min-spec", "inf"), ("--min-env", "2")):
            run(wav_tool, [tone, tone, option, value], 2)
        broken = work / "truncated.wav"
        broken.write_bytes(tone.read_bytes()[:-2])
        run(wav_tool, [tone, broken], 2)
        run(wav_tool, [tone, work / "absent.wav"], 2)

        left, right = work / "left", work / "right"
        left.mkdir()
        right.mkdir()
        for i in range(24):
            for folder in (left, right):
                grey_bmp(folder / f"f{i:04d}.bmp", 16, 16, [80 + i] * 256)
        run(ssim, [left, right, "--min-ssim", "1"], 0)
        run(ssim, [left, right, "--windows", "1", "--search", "--search-range", "2"], 0)
        for option, value in (("--sample", 0), ("--window", 0), ("--search-step", 0),
                              ("--min-ssim", "nan"), ("--worst", -1)):
            run(ssim, [left, right, option, value], 2)
        single = work / "single"
        single.mkdir()
        grey_bmp(single / "only.bmp", 16, 16, [0] * 256)
        run(ssim, [single, single, "--search"], 2)
        run(ssim, [left, right, "--offset", "1000"], 2)
        run(ssim, [left, work / "absent"], 2)
        # A differing frame must be detected, not averaged away.
        grey_bmp(right / "f0005.bmp", 16, 16, [(i * 7) % 256 for i in range(256)])
        report = run(ssim, [left, right, "--worst", "1", "--min-ssim", "0.99"], 1)
        assert "A frame     5" in report, report
        run(ssim, [left, right, "--min-ssim", "0.5"], 0)
        grey_bmp(right / "f0000.bmp", 8, 8, [0] * 64)
        run(ssim, [left, right], 2)
    print("PASS: WAV tones/silence/truncation and SSIM input/overlap boundaries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
