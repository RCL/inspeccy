#!/usr/bin/env python3
"""Generate original geometric NXI input and a byte-sequence VFS payload."""
import argparse
from pathlib import Path


def image():
    # NXI: 256 two-byte Next palette entries, then 320 columns of 256 pixels.
    palette = bytes(value for index in range(256) for value in (index, index & 1))
    pixels = bytes(((x // 16) + 5 * (y // 16)) % 32
                   for x in range(320) for y in range(256))
    return palette + pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    files = {"synthetic-320-pal.nxi": image(), "TEST.DAT": bytes(range(256)) * 1024}
    for name, data in files.items():
        path = args.out / name
        if args.check:
            if not path.is_file() or path.read_bytes() != data:
                print(f"Fixture mismatch: {path}")
                return 1
        else:
            args.out.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    print(f"{'Verified' if args.check else 'Wrote'} {len(files)} dot-command data fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
