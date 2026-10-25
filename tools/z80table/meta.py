"""Flag effects and register reads/writes per (mnemonic, operand-form).

An emulator computes flags; it does not tabulate them, so this data cannot be
extracted from inspeccy. It is written from documented references:

  - Zilog Z80 CPU User Manual (documented flag behaviour)
  - "The Undocumented Z80 Documented" (Sean Young), for X/Y and the block
    instructions
  - ZX Spectrum Next documentation for the Z80N extensions

and then CHECKED empirically by tools/verify_effects.py, which runs every
canonical row under inspeccy with random state and fails if a register
outside `writes` changed, a flag marked unaffected changed, or a flag marked
0/1 came out otherwise. Where a flag's behaviour is not documented well enough
to assert, it is marked '?' (unknown) and any assertion that depends on it
fails by construction.

Flag codes, in the order S Z Y H X P N C:
  '-' unaffected   '*' affected (computed)   '0' reset   '1' set   '?' unknown

Register names in reads/writes are 8-bit halves (A F B C D E H L IXH IXL IYH
IYL I R), 16-bit units SP and PC, IFF, and the shadow set as units AF' BC'
DE' HL'. `mem` says whether memory is read/written ('' 'r' 'w' 'rw').
"""

from __future__ import annotations

FLAG_ORDER = "SZYHXPNC"


class NoRule(Exception):
    pass


def _flags(s: str) -> dict[str, str]:
    """'**?1?*0-' -> dict in FLAG_ORDER."""
    if len(s) != 8:
        raise ValueError(s)
    return dict(zip(FLAG_ORDER, s))


NONE = _flags("--------")
ALL = _flags("********")

_PAIR = {"af": ("A", "F"), "bc": ("B", "C"), "de": ("D", "E"), "hl": ("H", "L"),
         "ix": ("IXH", "IXL"), "iy": ("IYH", "IYL")}
_R8 = {"a": "A", "b": "B", "c": "C", "d": "D", "e": "E", "h": "H", "l": "L",
       "i": "I", "r": "R", "ixh": "IXH", "ixl": "IXL", "iyh": "IYH", "iyl": "IYL"}


def _operand(tok: str) -> tuple[set[str], set[str], str, bool]:
    """(regs read for the value, regs read for addressing, mem access, is_memory)
    when the operand is a *source*. For a destination the value-regs become
    writes; addressing regs stay reads."""
    if tok in _R8:
        return {_R8[tok]}, set(), "", False
    if tok in _PAIR:
        return set(_PAIR[tok]), set(), "", False
    if tok == "sp":
        return {"SP"}, set(), "", False
    if tok == "af'":
        return {"AF'"}, set(), "", False
    if tok in ("(hl)", "(bc)", "(de)"):
        return set(), set(_PAIR[tok[1:3]]), "m", True
    if tok in ("(ix+d)", "(iy+d)"):
        return set(), set(_PAIR[tok[1:3]]), "m", True
    if tok == "(sp)":
        return set(), {"SP"}, "m", True
    if tok == "(nn)":
        return set(), set(), "m", True
    if tok == "(c)":            # port BC
        return set(), {"B", "C"}, "", False
    if tok in ("(ix)", "(iy)"):   # jp (ix): address, not memory
        return set(_PAIR[tok[1:3]]), set(), "", False
    if tok in ("n", "nn", "rel", "bit", "im", "rst", "(n)", "0"):
        return set(), set(), "", False
    if tok == "cc":
        return {"F"}, set(), "", False
    raise NoRule(f"operand form {tok!r}")


def _dst_src(forms: list[str]) -> tuple[set[str], set[str], str]:
    """reads, writes, mem for a 2-operand 'dst,src' instruction."""
    d_val, d_addr, d_mem, _ = _operand(forms[0])
    s_val, s_addr, s_mem, _ = _operand(forms[1])
    reads = s_val | s_addr | d_addr
    writes = set(d_val)
    mem = ("r" if s_mem else "") + ("w" if d_mem else "")
    return reads, writes, mem


def _rw(forms: list[str], idx: int = 0) -> tuple[set[str], set[str], str]:
    """reads, writes, mem for an instruction that reads and writes operand idx."""
    val, addr, m, _ = _operand(forms[idx])
    return val | addr, set(val), ("rw" if m else "")


def effects(mnemonic: str, forms: list[str]) -> dict:
    m = mnemonic
    f = list(forms)
    flags = NONE
    reads: set[str] = set()
    writes: set[str] = set()
    mem = ""

    if m == "ld":
        reads, writes, mem = _dst_src(f)
        if f[0] == "a" and f[1] in ("i", "r"):
            flags = _flags("***0*_0-".replace("_", "*"))
            reads |= {"IFF"}
    elif m in ("add", "adc", "sub", "sbc", "and", "xor", "or", "cp"):
        if f[0] == "a" or len(f) == 1:
            src = f[-1]
            s_val, s_addr, s_mem, _ = _operand(src)
            reads = {"A"} | s_val | s_addr
            writes = {"A", "F"} if m != "cp" else {"F"}
            mem = "r" if s_mem else ""
            if m in ("adc", "sbc"):
                reads |= {"F"}
            flags = {
                "add": _flags("******0*"),
                "adc": _flags("******0*"),
                "sub": _flags("******1*"),
                "sbc": _flags("******1*"),
                "cp":  _flags("******1*"),
                "and": _flags("***1**00"),
                "xor": _flags("***0**00"),
                "or":  _flags("***0**00"),
            }[m]
        elif f[0] in ("hl", "ix", "iy") and f[1] in ("bc", "de", "hl", "sp", "ix", "iy"):
            d = set(_PAIR[f[0]]) if f[0] != "sp" else {"SP"}
            s = set(_PAIR[f[1]]) if f[1] != "sp" else {"SP"}
            reads, writes = d | s, d | {"F"}
            if m == "add":
                flags = _flags("--***-0*")   # X and Y come from the high byte of the result
            elif m == "adc":
                flags = _flags("******0*")
                reads |= {"F"}
            else:
                flags = _flags("******1*")
                reads |= {"F"}
        elif m == "add" and f[0] in ("hl", "de", "bc") and f[1] in ("a", "nn"):
            d = set(_PAIR[f[0]])
            reads = d | ({"A"} if f[1] == "a" else set())
            writes = d
        else:
            raise NoRule(f"{m} {','.join(f)}")
    elif m in ("inc", "dec"):
        val, addr, mm, is_mem = _operand(f[0])
        reads, writes = val | addr, set(val)
        mem = "rw" if is_mem else ""
        if f[0] in _PAIR or f[0] == "sp":
            flags = NONE
        else:
            writes |= {"F"}
            flags = _flags("******0-") if m == "inc" else _flags("******1-")
    elif m == "push":
        if f[0] == "nn":
            reads, writes, mem = {"SP"}, {"SP"}, "w"
        else:
            val, _, _, _ = _operand(f[0])
            reads, writes, mem = val | {"SP"}, {"SP"}, "w"
    elif m == "pop":
        val, _, _, _ = _operand(f[0])
        reads, writes, mem = {"SP"}, val | {"SP"}, "r"
        if f[0] == "af":
            flags = ALL
    elif m == "ex":
        if f == ["de", "hl"]:
            reads = writes = {"D", "E", "H", "L"}
        elif f == ["af", "af'"]:
            reads = writes = {"A", "F", "AF'"}
            flags = ALL
        elif f[0] == "(sp)":
            val = set(_PAIR[f[1]])
            reads, writes, mem = val | {"SP"}, set(val), "rw"
        else:
            raise NoRule(f"ex {','.join(f)}")
    elif m == "exx":
        reads = writes = {"B", "C", "D", "E", "H", "L", "BC'", "DE'", "HL'"}
    elif m in ("ldi", "ldd", "ldir", "lddr"):
        reads = writes = {"H", "L", "D", "E", "B", "C"}
        writes = writes | {"F"}
        mem = "rw"
        flags = _flags("--?0?*0-")
    elif m in ("cpi", "cpd", "cpir", "cpdr"):
        reads = {"A", "H", "L", "B", "C"}
        writes = {"H", "L", "B", "C", "F"}
        mem = "r"
        flags = _flags("**?*?*1-")
    elif m in ("ini", "ind", "inir", "indr"):
        reads = {"B", "C", "H", "L"}
        writes = {"H", "L", "B", "F"}
        mem = "w"
        flags = _flags("**??????")
    elif m in ("outi", "outd", "otir", "otdr"):
        reads = {"B", "C", "H", "L"}
        writes = {"H", "L", "B", "F"}
        mem = "r"
        flags = _flags("**??????")
    elif m == "outinb":
        reads = {"B", "C", "H", "L"}
        writes = {"H", "L", "F"}
        mem = "r"
        flags = _flags("????????")
    elif m == "jp":
        if f[0] == "cc":
            reads = {"F"}
        elif f[0] in ("(hl)", "(ix)", "(iy)"):
            reads = set(_PAIR[f[0][1:3]])
        elif f[0] == "(c)":
            reads = {"B", "C"}
        writes = {"PC"}
    elif m == "jr":
        reads = {"F"} if f[0] == "cc" else set()
        writes = {"PC"}
    elif m == "djnz":
        reads, writes = {"B"}, {"B", "PC"}
    elif m == "call":
        reads = ({"F"} if f[0] == "cc" else set()) | {"SP"}
        writes, mem = {"SP", "PC"}, "w"
    elif m == "rst":
        reads, writes, mem = {"SP"}, {"SP", "PC"}, "w"
    elif m == "ret":
        reads = ({"F"} if f else set()) | {"SP"}
        writes, mem = {"SP", "PC"}, "r"
    elif m in ("reti", "retn"):
        reads, writes, mem = {"SP", "IFF"}, {"SP", "PC", "IFF"}, "r"
    elif m in ("rlca", "rrca", "rla", "rra"):
        reads, writes = {"A"} | ({"F"} if m in ("rla", "rra") else set()), {"A", "F"}
        flags = _flags("--*0*-0*")
    elif m in ("rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"):
        reads, writes, mem = _rw(f, 0)
        writes |= {"F"}
        if m in ("rl", "rr"):
            reads |= {"F"}
        flags = _flags("***0**0*")
    elif m == "bit":
        val, addr, mm, is_mem = _operand(f[1])
        reads, writes, mem = val | addr, {"F"}, ("r" if is_mem else "")
        flags = _flags("?*?1??0-")
    elif m in ("res", "set"):
        reads, writes, mem = _rw(f, 1)
    elif m == "daa":
        reads, writes = {"A", "F"}, {"A", "F"}
        flags = _flags("******-*")
    elif m == "cpl":
        reads, writes = {"A"}, {"A", "F"}
        flags = _flags("--*1*-1-")
    elif m == "neg":
        reads, writes = {"A"}, {"A", "F"}
        flags = _flags("******1*")
    elif m == "ccf":
        reads, writes = {"A", "F"}, {"F"}
        flags = _flags("--***-0*")
    elif m == "scf":
        reads, writes = {"A", "F"}, {"F"}
        flags = _flags("--*0*-01")
    elif m in ("nop", "halt"):
        pass
    elif m in ("di", "ei"):
        writes = {"IFF"}
    elif m == "im":
        pass
    elif m == "in":
        if f == ["a", "(n)"]:
            writes = {"A"}
        elif f == ["(c)"] or (len(f) == 2 and f[1] == "(c)"):
            reads = {"B", "C"}
            if len(f) == 1 or f[0] == "f":
                writes = {"F"}          # in (c) / in f,(c): flags only
            else:
                writes = {_R8[f[0]], "F"}
            flags = _flags("***0**0-")
        else:
            raise NoRule(f"in {','.join(f)}")
    elif m == "out":
        if f[0] == "(n)":
            reads = {"A"}
        elif f[0] == "(c)":
            reads = {"B", "C"} | ({_R8[f[1]]} if f[1] in _R8 else set())
        else:
            raise NoRule(f"out {','.join(f)}")
    elif m in ("rld", "rrd"):
        reads, writes, mem = {"A", "H", "L"}, {"A", "F"}, "rw"
        flags = _flags("***0**0-")
    # ---- Z80N
    elif m == "swapnib":
        reads = writes = {"A"}
    elif m == "mirror":
        reads = writes = {"A"}
    elif m == "test":
        reads, writes = {"A"}, {"F"}
        flags = _flags("***1**00")
    elif m == "mul":
        reads = writes = {"D", "E"}
    elif m in ("bsla", "bsra", "bsrl", "bsrf", "brlc"):
        reads, writes = {"D", "E", "B"}, {"D", "E"}
    elif m == "nextreg":
        reads = {"A"} if f[1] == "a" else set()
    elif m == "pixeldn":
        reads = writes = {"H", "L"}
    elif m == "pixelad":
        reads, writes = {"D", "E"}, {"H", "L"}
    elif m == "setae":
        reads, writes = {"E"}, {"A"}
    elif m in ("ldix", "lddx", "ldirx", "lddrx", "ldpirx"):
        reads = {"A", "H", "L", "D", "E", "B", "C"}
        writes = {"H", "L", "D", "E", "B", "C"}
        mem = "rw"
        flags = _flags("????????")
        writes |= {"F"}
    elif m == "ldws":
        # Next docs: flags as `inc d`. inspeccy leaves F untouched (measured by
        # tools/verify_effects.py), so the behaviour is uncertain: unknown.
        reads, writes, mem = {"H", "L", "D", "E"}, {"L", "D", "F"}, "rw"
        flags = _flags("????????")
    else:
        raise NoRule(f"{m} {','.join(f)}")

    if any(v in "*01?" for v in flags.values()):
        writes = set(writes) | {"F"}
    return {"flags": dict(flags), "reads": set(reads), "writes": set(writes), "mem": mem}
