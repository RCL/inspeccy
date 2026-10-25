#!/usr/bin/env python3
"""Compare opcode disassembly and timing with the bundled frozen references.

Each byte pattern runs with three flag/counter states to exercise conditional
and repeating instructions. This is a regression check against the recorded
emulator behavior; it does not prove flag results or hardware accuracy.
Uses only generated RAM snapshots and a zero-filled placeholder ROM.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from test_support import locate_test_binary

REFS = Path(__file__).resolve().parents[1] / 'refs' / 'z80_fingerprint'
TRACE = re.compile(r'^\s*(\d+)\s*\|\s*\d+\s*\|\s*\$([0-9A-Fa-f]{4})\s*\([^)]*\)\s*\|\s*([^|]*?)\s*(?:\||$)')
VARIANTS = ((0x1200, 0x0205), (0x12ff, 1), (0x12ff, 0x0105))


def snapshot(code, af, bc):
    ram = bytearray(49152)
    ram[0x4000:0x4000 + len(code)] = bytes(code)
    # Match the recorded register setup; the SNA loader pops PC from $fefc.
    ram[0xbefc:0xbefe] = b'\x00\x80'
    header = struct.pack('<BHHHHHHHHHBBHHBB', 0x3f, 0, 0, 0, 0,
                         0xc000, 0xd000, bc, 0xc200, 0xc100, 0, 0, af, 0xfefc, 1, 7)
    return header + ram


def measure(exe, machine, code, af, bc, work):
    (work / 'case.sna').write_bytes(snapshot(code, af, bc))
    trace = work / 'case.trace'
    trace.unlink(missing_ok=True)
    command = [str(exe), '--sna', 'case.sna', '--machine', machine, '--rom48', 'unused.rom',
               '--cycles', '400', '--no-interrupts', '--contention', 'off',
               '--trace-out', str(trace), '--trace-format', 'minimal',
               '--out', 'out.sna', '--no-bmp', '--no-status', '--no-dump']
    result = subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=15)
    if result.returncode not in (0, 1, 2, 5, 6) or not trace.is_file():
        raise RuntimeError(result.stdout + result.stderr)
    lines = [m.groups() for line in trace.read_text().splitlines() if (m := TRACE.match(line))]
    if not lines or int(lines[0][1], 16) != 0x8000:
        raise RuntimeError('missing initial instruction in trace: ' + result.stdout)
    if len(lines) >= 2:
        ticks = int(lines[1][0]) - int(lines[0][0])
    else:
        total = re.search(r'\bcycles=(\d+)', result.stdout)
        if not total:
            raise RuntimeError('missing cycle summary: ' + result.stdout)
        ticks = int(total[1]) - int(lines[0][0])
    return lines[0][2].strip().lower(), ticks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspeccy', type=Path)
    parser.add_argument('--machines', default='next50,48k')
    parser.add_argument('--filter', default='', help='Only row IDs containing this text')
    args = parser.parse_args()
    exe = args.inspeccy.resolve() if args.inspeccy else locate_test_binary()
    failures = []
    with tempfile.TemporaryDirectory(prefix='inspeccy-opcodes-') as directory:
        work = Path(directory)
        (work / 'unused.rom').write_bytes(bytes(16384))
        for machine in args.machines.split(','):
            path = REFS / f'{machine}.json'
            if machine not in ('48k', 'next50') or not path.is_file():
                raise RuntimeError(f'No frozen reference for {machine}')
            rows = [row for row in json.loads(path.read_text())['rows'] if args.filter in row['id']]
            if not rows:
                raise RuntimeError('No opcode rows selected')
            for index, row in enumerate(rows):
                observed = set()
                for af, bc in VARIANTS:
                    text, ticks = measure(exe, machine, row['bytes'], af, bc, work)
                    if text != row['disasm'].lower():
                        failures.append(f"{machine} {row['id']}: {text!r} != {row['disasm']!r}")
                    observed.add(ticks)
                expected = set(row['tstates'] if isinstance(row['tstates'], list) else [row['tstates']])
                if observed != expected:
                    failures.append(f"{machine} {row['id']}: timing {sorted(observed)} != {sorted(expected)}")
                if (index + 1) % 400 == 0:
                    print(f'{machine}: {index + 1}/{len(rows)} rows checked', flush=True)
            print(f'{machine}: {len(rows)} opcode patterns checked', flush=True)
    for failure in failures[:100]:
        print(failure)
    print(f'z80_fingerprint: {len(failures)} differences')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
