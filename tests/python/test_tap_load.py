"""Cold-boot a generated TAP through the standard 128K ROM loader."""
import subprocess
import tempfile
from pathlib import Path
from boot_fixture import check_run, write_tap
from test_support import locate_test_binary, test_rom


def main():
    rom = test_rom('128.rom')
    if not rom.is_file():
        print('tap_load: SKIP (provide 128.rom through INSPECCY_ROM_DIR)')
        return 77
    binary = locate_test_binary()
    with tempfile.TemporaryDirectory(prefix='inspeccy-tap-') as temporary:
        folder = Path(temporary)
        tape, snapshot = folder / 'boot.tap', folder / 'boot.sna'
        write_tap(tape)
        result = subprocess.run([str(binary), '--tap', str(tape), '--machine', '128k',
                                 '--rom48128', str(rom), '--cycles', str(700 * 70908),
                                 '--no-bmp', '--no-dump', '--no-status', '--out', str(snapshot)],
                                capture_output=True, text=True)
        check_run(result, snapshot)
    print('tap_load: PASS (autostart BASIC, RAM signature, screen fill, AY writes)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
