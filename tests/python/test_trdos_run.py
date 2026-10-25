"""Boot generated TR-DOS BASIC files by name, including a dotted filename."""
import subprocess
import tempfile
from pathlib import Path
from boot_fixture import check_run, write_trd
from test_support import locate_test_binary, test_rom


def main():
    rom, trdos = test_rom('128.rom'), test_rom('trdos504t.rom')
    if not rom.is_file() or not trdos.is_file():
        print('trdos_run: SKIP (provide 128.rom and trdos504t.rom through INSPECCY_ROM_DIR)')
        return 77
    binary = locate_test_binary()
    counts = []
    with tempfile.TemporaryDirectory(prefix='inspeccy-trd-') as temporary:
        folder = Path(temporary)
        for name in ('BootTest', 'Boot.Tst'):
            disk, snapshot = folder / (name + '.trd'), folder / (name + '.sna')
            write_trd(disk, name)
            result = subprocess.run([str(binary), '--trd', str(disk), '--trd-run', name,
                                     '--machine', '128k', '--rom48128', str(rom),
                                     '--rom-trdos', str(trdos), '--cycles', str(1500 * 70908),
                                     '--no-bmp', '--no-dump', '--no-status', '--out', str(snapshot)],
                                    capture_output=True, text=True)
            counts.append(check_run(result, snapshot))
    assert counts[0] == counts[1], 'dotted filename changed the boot result'
    print('trdos_run: PASS (both names, RAM signature, screen fill, AY writes)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
