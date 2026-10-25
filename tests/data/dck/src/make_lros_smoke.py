"""python make_lros_smoke.py  ->  ../lros_smoke.dck

Builds the cartridge fixture for cases/dck_lros.test. Two DOCK chunks:

    chunk 0   the LROS (lros_smoke.asm), padded to 8K
    chunk 1   a single marker byte, $5A, which the LROS pages in and paints the
              screen with. If dock paging is broken the read returns open-bus
              $FF instead and the expected bitmap byte does not match, so the
              test fails on the pixels rather than passing for the wrong reason.

Re-run this only if lros_smoke.asm changes; the .dck is checked in.
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "lros_smoke.dck"

CHUNK = 8192
MARKER = 0x5A

BANK_DOCK = 0
KIND_ABSENT, KIND_RAM_BLANK, KIND_ROM, KIND_RAM = 0, 1, 2, 3


def main() -> int:
    raw = HERE / "lros_smoke.bin"
    r = subprocess.run(["sjasmplus", "--nologo", f"--raw={raw.as_posix()}",
                        (HERE / "lros_smoke.asm").as_posix()],
                       capture_output=True, text=True, cwd=HERE)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        return 1

    code = raw.read_bytes()
    if len(code) > CHUNK:
        print(f"chunk 0 overflow: {len(code)} bytes", file=sys.stderr)
        return 1

    chunk0 = bytearray(b"\xff" * CHUNK)
    chunk0[0:len(code)] = code
    chunk1 = bytearray(b"\x00" * CHUNK)
    chunk1[0] = MARKER

    # One block: bank id, eight kind bytes, then the populated chunks in order.
    out = bytearray([BANK_DOCK])
    kinds = [KIND_ROM, KIND_ROM] + [KIND_ABSENT] * 6
    out += bytes(kinds)
    out += chunk0
    out += chunk1

    OUT.write_bytes(bytes(out))
    raw.unlink(missing_ok=True)
    print(f"wrote {OUT} ({len(out)} bytes, LROS {len(code)} bytes, "
          f"marker ${MARKER:02X})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
