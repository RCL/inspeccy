"""The Z80/Z80N instruction knowledge, next to the emulator that verifies it.

meta.py          documented register and flag effects per (mnemonic, operand forms)
extract_table.py observes every opcode in inspeccy (encoding, timing, disassembly),
                 round-trips it through sjasmplus, attaches the effects: writes
                 tests/refs/z80_fingerprint/<machine>.json
verify_effects.py runs each instruction from random states and checks the effects
runner.py        the inspeccy CLI driver and artifact parsers
sna.py           snapshot writers

Consumers: ../gen_z80_tables.py (inspeccy's own header), ../gen_z80_effects.py (the
sjasmplus fork's contract checker) and tests/python/test_z80_fingerprint.py.
"""
