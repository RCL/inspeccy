"""Reject an emulator that clears OUTD carry and traps a program in its AY loop.

Runs synthetic RAM-only snapshots; no copyrighted ROM is required.
Reference for single (non-repeating) OUTD:
https://github.com/redcode/Z80/blob/master/sources/Z80.c (OUTX / INX_OUTX_COMMON).
"""
from pathlib import Path
import argparse
import json
import shutil
import struct
import subprocess
import tempfile


def check(exe, verbose=True):
    failures = []
    with tempfile.TemporaryDirectory(prefix='outd-check-') as directory:
        work = Path(directory)
        (work/'unused.rom').write_bytes(bytes(16384))
        cases = [(0x4a, 0, 0xbf), (3, 13, 0xbf)]
        cases += [(v, l, b) for v in (0, 0x7f, 0x80, 0xff)
                  for l in (0, 1, 0x81, 0xff) for b in (0, 1, 0x80)]
        cases.append((0x34, 1, 0xc0))  # C0FD must become BFFD before OUT (AY data).
        for i, (value, lo, before_b) in enumerate(cases):
            port_lo = 0xfd if i == len(cases)-1 else 0xff
            ram = bytearray(49152)
            ram[0x4000:0x4002] = b'\xed\xab'
            ram[0x5000+lo] = value
            ram[0xbefc:0xbefe] = b'\x00\x80'
            header = struct.pack('<BHHHHHHHHHBBHHBB', 0x3f, 0, 0, 0, 0,
                                 0x9000+lo, 0, (before_b<<8)|port_lo,
                                 0, 0, 0, 0, 0x55ff, 0xfefc, 1, 7)
            (work/'case.sna').write_bytes(header+ram)
            command = [str(exe), '--sna', str(work/'case.sna'), '--machine', '48k',
                       '--rom48', str(work/'unused.rom'), '--cycles', '16',
                       '--no-interrupts', '--contention', 'off', '--no-bmp',
                       '--out', str(work/'out.sna'), '--status-out', str(work/'status.json'),
                       '--dump-out', str(work/'dump.json')]
            result = subprocess.run(command, capture_output=True, text=True, timeout=20)
            status = json.loads((work/'status.json').read_text())
            assert status['exit']['result'] == 'BudgetReached', result.stdout+result.stderr
            report = json.loads((work/'dump.json').read_text())
            cpu = report['cpu']
            b = (before_b-1)&255
            total = value + ((lo-1)&255)
            flags = b&0xa8
            flags |= 0x40 if b == 0 else 0
            flags |= 2 if value&128 else 0
            flags |= 0x11 if total > 255 else 0
            flags |= 4 if (bin((total&7)^b).count('1')%2 == 0) else 0
            observed = int(cpu['AF'][1:], 16)&255
            if observed != flags:
                failures.append(f'V={value:02X} L={lo:02X} B={before_b:02X}: '
                                f'F={observed:02X}, expected {flags:02X}')
            assert cpu['PC'] == '$8002'
            assert int(cpu['HL'][1:],16) == 0x9000+lo-1
            assert int(cpu['BC'][1:],16) == (b<<8)|port_lo
            assert int(cpu['AF'][1:],16)>>8 == 0x55
            if port_lo == 0xfd:
                assert report['ay']['registers'][0] == '$34', 'OUTD used B before decrement'
        if failures:
            raise AssertionError('OUTD flag emulation is incorrect; the unmodified intro '
                                 'cannot run correctly.\n'+'\n'.join(failures[:5]))
        if verbose:
            print(f'OUTD prerequisite: {len(cases)} flag cases and decremented I/O address pass.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspeccy', default='inspeccy')
    args = parser.parse_args()
    check(Path(shutil.which(args.inspeccy) or args.inspeccy).resolve())
