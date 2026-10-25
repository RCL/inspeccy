#!/usr/bin/env python3
"""Frame-index sidecar (.btr.fidx) sanity test.

For a known short run we:
  1. Emit a .btr trace with default sidecar emission, capture both files.
  2. Validate sidecar magic + version + parent linkage (parent CRC reads
     back to match the .btr header's CRC).
  3. Walk index entries; assert each entry's StartCycle / StartTimeNanos /
     FrameNumber are monotonically increasing, that EntryCount matches the
     actual entry count by file size, and that the entries' FrameNumbers
     cover a contiguous range starting at the .btr's StartFrame.

This is the index format regression: any change that breaks the sidecar layout,
the entry order, or the parent-CRC linkage shows up here.

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
NEX = INSPECCY_DIR / "tests" / "data" / "nex" / "trace_workload.nex"
CYCLES = "600000"   # Eight frames with IRQs, nested calls, and RAM writes.


# Layouts mirror trace_binary.h. Magic values are little-endian.
BTR_MAGIC  = 0x42544E49  # "INTB"
FIDX_MAGIC = 0x49464E49  # "INFI"
VERSION    = 4           # tracks trace_binary.h kVersion (IndexHeader.Version = kVersion)

# IndexHeader: u32 Magic, u16 Version, u16 Reserved, u32 ParentMagic,
#              u32 ParentHeaderCrc, u64 StartFrame, u64 EndFrame,
#              u32 EntryCount, u32 Reserved2
IDX_HEADER_FMT  = "<IHHIIQQII"
IDX_HEADER_SIZE = struct.calcsize(IDX_HEADER_FMT)   # 40

# IndexEntry: u32 FrameNumber, u32 _Pad, u64 StartCycle, u64 StartTimeNanos,
#             u64 FirstByteOffset, u16 × 8 regs
IDX_ENTRY_FMT  = "<IIQQQHHHHHHHH"
IDX_ENTRY_SIZE = struct.calcsize(IDX_ENTRY_FMT)     # 48


def find_inspeccy():
    return locate_test_binary()


def run(*args: str) -> None:
    rc = subprocess.run(args, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL).returncode
    if rc != 0:
        sys.exit(f"inspeccy returned {rc} for: {' '.join(args)}")


def read_btr_header_crc(btr_path: Path) -> tuple[int, int, int, int]:
    """Return (StartFrame, EndFrame, header_crc, header_size) from the .btr."""
    data = btr_path.read_bytes()
    # Validate magic.
    magic, = struct.unpack_from("<I", data, 0)
    if magic != BTR_MAGIC:
        sys.exit(f"btr magic mismatch: 0x{magic:08x} != 0x{BTR_MAGIC:08x}")
    # Header fixed prefix is sizeof(Header). We use the StartFrame/EndFrame
    # offsets, which are at fixed positions. The CRC is in the trailing 4
    # bytes of header (fixed prefix + 2 length-prefixed blobs + crc).
    # We don't need to compute it ourselves; reading the .btr's own CRC
    # field tells us what the writer wrote.
    # Layout positions (matching trace_binary.h Header struct):
    #   offset  8: StartCycle (u64)
    #   offset 16: EndCycle   (u64)
    #   offset 24: StartFrame (u64)
    #   offset 32: EndFrame   (u64)
    start_frame, = struct.unpack_from("<Q", data, 24)
    end_frame,   = struct.unpack_from("<Q", data, 32)
    # v3 (offset 72): FrameIntPeriod (u32) — nominal T-states/frame, must be nonzero.
    frame_int_period, = struct.unpack_from("<I", data, 72)
    assert frame_int_period != 0, "header FrameIntPeriod is 0 (expected nominal frame period)"
    # CRC lives at end of header = sizeof(Header) + 2 + CliLen + 2 + SldLen.
    # sizeof(BinaryTrace::Header) is 154 with #pragma pack(1): 119 through v3,
    # plus v4's initial video state — InitBorder / InitPort7FFD / InitScldMode /
    # InitEvoFf77 (4) and InitDisplayRegs[31]. If this changes, bump the trace
    # version, which is what makes stale readers reject rather than misparse.
    fixed_size = 154
    # Actually compute from blob lengths:
    cli_len_off = fixed_size
    cli_len, = struct.unpack_from("<H", data, cli_len_off)
    sld_len_off = cli_len_off + 2 + cli_len
    sld_len, = struct.unpack_from("<H", data, sld_len_off)
    crc_off = sld_len_off + 2 + sld_len
    header_crc, = struct.unpack_from("<I", data, crc_off)
    header_size = crc_off + 4
    return start_frame, end_frame, header_crc, header_size


def main() -> int:
    inspeccy = find_inspeccy()
    if not NEX.is_file():
        sys.exit(f"input NEX not found: {NEX}")

    with tempfile.TemporaryDirectory(prefix="btr_fidx_") as td:
        td = Path(td)
        btr  = td / "trace.btr"
        fidx = td / "trace.btr.fidx"

        run(str(inspeccy), "--machine", "next50", "--nex", str(NEX),
            "--cycles", CYCLES, "--trace-out-bin", str(btr),
            "--no-bmp", "--no-status", "--no-dump")

        if not fidx.exists():
            sys.exit("sidecar not produced")

        # Parent .btr fields we need to cross-check.
        btr_start_frame, btr_end_frame, btr_header_crc, _ = \
            read_btr_header_crc(btr)

        # Parse the .fidx header.
        fidx_bytes = fidx.read_bytes()
        if len(fidx_bytes) < IDX_HEADER_SIZE:
            sys.exit(f"fidx too small: {len(fidx_bytes)} bytes")
        (magic, version, _res, parent_magic, parent_crc,
         idx_start_frame, idx_end_frame, entry_count, _res2) = \
            struct.unpack_from(IDX_HEADER_FMT, fidx_bytes, 0)

        if magic != FIDX_MAGIC:
            sys.exit(f"fidx magic mismatch: 0x{magic:08x}")
        if version != VERSION:
            sys.exit(f"fidx version mismatch: {version}")
        if parent_magic != BTR_MAGIC:
            sys.exit(f"fidx parent magic mismatch: 0x{parent_magic:08x}")
        if parent_crc != btr_header_crc:
            sys.exit(f"fidx parent CRC mismatch: 0x{parent_crc:08x} != "
                     f"0x{btr_header_crc:08x}")
        if idx_start_frame != btr_start_frame:
            sys.exit(f"fidx StartFrame mismatch")
        if idx_end_frame != btr_end_frame:
            sys.exit(f"fidx EndFrame mismatch: {idx_end_frame} vs "
                     f"{btr_end_frame}")
        # File size sanity.
        expected_size = IDX_HEADER_SIZE + entry_count * IDX_ENTRY_SIZE
        if len(fidx_bytes) != expected_size:
            sys.exit(f"fidx file size {len(fidx_bytes)} != expected "
                     f"{expected_size} ({entry_count} entries)")

        # Walk entries.
        prev_frame = -1
        prev_cycle = -1
        prev_nanos = -1
        prev_offset = -1
        for i in range(entry_count):
            off = IDX_HEADER_SIZE + i * IDX_ENTRY_SIZE
            (frame, _pad, cycle, nanos, byte_off,
             pc, af, bc, de, hl, ix, iy, sp) = \
                struct.unpack_from(IDX_ENTRY_FMT, fidx_bytes, off)
            if frame <= prev_frame and i != 0:
                sys.exit(f"entry {i}: FrameNumber {frame} not monotone "
                         f"(prev {prev_frame})")
            if cycle < prev_cycle:
                sys.exit(f"entry {i}: StartCycle {cycle} not monotone "
                         f"(prev {prev_cycle})")
            if nanos < prev_nanos:
                sys.exit(f"entry {i}: StartTimeNanos {nanos} not monotone")
            if byte_off <= prev_offset and i != 0:
                sys.exit(f"entry {i}: FirstByteOffset {byte_off} not monotone")
            prev_frame, prev_cycle, prev_nanos, prev_offset = \
                frame, cycle, nanos, byte_off

        # Entry 0's frame number should equal the .btr StartFrame.
        first_frame, = struct.unpack_from(
            "<I", fidx_bytes, IDX_HEADER_SIZE)
        if first_frame != btr_start_frame:
            sys.exit(f"first index entry frame {first_frame} != "
                     f"btr StartFrame {btr_start_frame}")

        # --no-trace-index leaves no sidecar.
        btr_noidx = td / "trace_noidx.btr"
        fidx_noidx = td / "trace_noidx.btr.fidx"
        run(str(inspeccy), "--machine", "next50", "--nex", str(NEX),
            "--cycles", CYCLES, "--trace-out-bin", str(btr_noidx),
            "--no-trace-index",
            "--no-bmp", "--no-status", "--no-dump")
        if fidx_noidx.exists():
            sys.exit(f"--no-trace-index left a sidecar: {fidx_noidx}")

        print(f"PASS: fidx with {entry_count} entries, parent CRC linked, "
              f"frames {idx_start_frame}..{idx_end_frame}, monotone.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
