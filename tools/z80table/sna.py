"""SNA snapshot writer. inspeccy has no --poke/--reg flags: the supported way to
set machine state is to hand it a snapshot, so every run starts from one built
here. 48K (49179 bytes) and 128K (131103 bytes) layouts.

Register dict keys (all optional, default 0): AF BC DE HL AFp BCp DEp HLp IX IY
SP PC I R IFF2 IM border. PC is delivered the way the format demands - pushed on
the stack - so SP must point at writable RAM with two free bytes below it.
"""

from __future__ import annotations

import struct
from pathlib import Path

REG16 = ("AF", "BC", "DE", "HL", "AFp", "BCp", "DEp", "HLp", "IX", "IY", "SP", "PC")


def _hdr(regs: dict, sp_after_push: int) -> bytes:
    g = lambda k: int(regs.get(k, 0)) & 0xFFFF  # noqa: E731
    iff2 = 0x04 if regs.get("IFF2", 0) else 0
    return struct.pack(
        "<BHHHHHHHHHBBHHBB",
        int(regs.get("I", 0)) & 0xFF,
        g("HLp"), g("DEp"), g("BCp"), g("AFp"),
        g("HL"), g("DE"), g("BC"), g("IY"), g("IX"),
        iff2, int(regs.get("R", 0)) & 0xFF,
        g("AF"), sp_after_push,
        int(regs.get("IM", 1)) & 0xFF, int(regs.get("border", 7)) & 0x07,
    )


def write_sna48(path: Path | str, regs: dict, ram: bytes | bytearray) -> Path:
    """ram = 49152 bytes covering $4000..$FFFF."""
    if len(ram) != 49152:
        raise ValueError(f"48K SNA needs 49152 bytes of RAM, got {len(ram)}")
    ram = bytearray(ram)
    sp = int(regs.get("SP", 0xFF00)) & 0xFFFF
    pc = int(regs.get("PC", 0x8000)) & 0xFFFF
    sp2 = (sp - 2) & 0xFFFF
    if sp2 < 0x4000 or sp2 > 0xFFFE:
        raise ValueError(f"SP={sp:#06x} must leave two bytes of RAM below it for the PC push")
    ram[sp2 - 0x4000] = pc & 0xFF
    ram[sp2 + 1 - 0x4000] = pc >> 8
    r = dict(regs)
    r["SP"] = sp2
    path = Path(path)
    path.write_bytes(_hdr(r, sp2) + bytes(ram))
    return path


def write_sna128(path: Path | str, regs: dict, banks: list[bytes], port_7ffd: int = 0x10,
                 trdos: int = 0) -> Path:
    """banks = eight 16K images (bank 0..7). The paged-in bank at $C000 is
    port_7ffd & 7; the 48K block of the file is banks 5, 2, and that one.
    """
    if len(banks) != 8 or any(len(b) != 16384 for b in banks):
        raise ValueError("128K SNA needs eight 16384-byte banks")
    banks = [bytearray(b) for b in banks]
    sp = int(regs.get("SP", 0xFF00)) & 0xFFFF
    pc = int(regs.get("PC", 0x8000)) & 0xFFFF
    cur = port_7ffd & 7

    def poke(addr: int, v: int) -> None:
        if addr < 0x4000:
            raise ValueError("cannot write ROM in a snapshot")
        b, off = {1: (5, addr - 0x4000), 2: (2, addr - 0x8000), 3: (cur, addr - 0xC000)}[addr >> 14]
        banks[b][off] = v

    # 128K format carries PC in the header; nothing is pushed, SP is as given.
    hdr = _hdr(regs, sp)
    body = bytes(banks[5]) + bytes(banks[2]) + bytes(banks[cur])
    ext = struct.pack("<HBB", pc, port_7ffd & 0xFF, trdos & 0xFF)
    rest = b"".join(bytes(banks[i]) for i in range(8) if i not in (5, 2, cur))
    path = Path(path)
    path.write_bytes(hdr + body + ext + rest)
    return path


class Ram48:
    """Convenience: a 48K RAM image ($4000..$FFFF) with poke helpers."""

    def __init__(self, fill: int = 0):
        self.data = bytearray([fill & 0xFF]) * 49152

    def poke(self, addr: int, data: bytes | bytearray | list[int] | int) -> None:
        if isinstance(data, int):
            data = bytes([data & 0xFF])
        data = bytes(data)
        if addr < 0x4000 or addr + len(data) > 0x10000:
            raise ValueError(f"poke {addr:#06x}+{len(data)} outside RAM $4000..$FFFF")
        self.data[addr - 0x4000:addr - 0x4000 + len(data)] = data

    def peek(self, addr: int, n: int = 1) -> bytes:
        if addr < 0x4000 or addr + n > 0x10000:
            raise ValueError(f"peek {addr:#06x}+{n} outside RAM")
        return bytes(self.data[addr - 0x4000:addr - 0x4000 + n])

    def bytes(self) -> bytes:
        return bytes(self.data)
