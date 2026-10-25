# z80table

One place for what the tree knows about Z80 and Z80N instructions, kept next to
the emulator because the emulator is what verifies it.

`tests/refs/z80_fingerprint/next50.json` (and `48k.json`) is the table: 1631
byte patterns across the base, CB, ED, DD, FD, DDCB and FDCB pages, each with
its canonical syntax, encoding, size, M1 count, T-states (taken/not-taken or
repeat/last where that differs), the registers it reads and writes, and the
flags it writes. Nothing in it is typed in from memory:

- **Encoding, disassembly, timing** are observed: `extract_table.py` puts each
  pattern at $8000 in a snapshot, runs it for one instruction in inspeccy under
  three register states, reads the trace, and assembles the disassembly back
  with sjasmplus to tell canonical rows from alias encodings.
- **Register and flag effects** come from documentation (`meta.py`, from the
  Zilog manual, "The Undocumented Z80 Documented" and the Next Z80N notes) and
  are then checked empirically by `verify_effects.py`: a register that changed
  must be in `writes`; a flag marked unaffected must not move; a flag marked
  0/1 must come out so.

## Consumers

| who | what it takes |
|---|---|
| `tools/gen_z80_tables.py` | encodings and timings for inspeccy's peephole model (`core/src/z80_tables_generated.h`) |
| `tools/gen_z80_effects.py` | reads/writes/flags/timing for the sjasmplus fork's contract checker (`sjasmplus/sjasm/z80_effects_generated.h`) |
| `tests/python/test_z80_fingerprint.py` | the emulator must still disassemble and time every pattern as recorded |

The emulator header is checked here by the test suite. The optional effects
header belongs to its external consumer; pass its path with `--out`, or set
`INSPECCY_EFFECTS_HEADER` to include its freshness check in the suite.

## Regenerating

```
python tools/z80table/extract_table.py              # ~5 min, writes next50.json
python tools/z80table/extract_table.py --machine 48k
python tools/z80table/verify_effects.py             # ~5 min, reports effect rows the emulator contradicts
python tools/gen_z80_tables.py
python tools/gen_z80_effects.py --out path/to/z80_effects_generated.h
```

Regenerate when the emulator's instruction timing changes, when `meta.py`
gains or corrects a rule, or when a new Z80N opcode is added. A changed row is
a finding about the emulator or about the documentation, and the history of
`tests/refs/z80_fingerprint/` is where those findings are kept.

