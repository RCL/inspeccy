"""Run the SDL frontend headlessly: Copper split must survive to its output.

The tiny original NEX uses only bank 40, changes turbo during boot, keeps
interrupts disabled, and changes fallback colour at paper lines 0/96/192.
No game assets, assembler, ROM or display server are required.
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def fixture(path):
    code = bytearray([0xf3])  # DI: beam finalisation must not depend on IRQ acceptance
    def nr(reg, value):
        code.extend([0xed, 0x91, reg, value])
    for reg, value in [(7, 3), (0x68, 0x80), (0x69, 0), (0x6b, 0), (0x15, 0),
                       (0x4a, 0xe0), (0x62, 0), (0x61, 0)]:
        nr(reg, value)
    copper = [0x4a, 0xe0, 0x80, 0, 0x4a, 0x1c, 0x80, 96, 0x4a, 3,
              0x80, 192, 0x4a, 0xe0, 0xff, 0xff]
    for byte in copper:
        nr(0x60, byte)
    nr(0x62, 0xc0)
    loop = 0xc000 + len(code)
    code.extend([0xc3, loop & 255, loop >> 8])
    header = bytearray(512)
    header[:8] = b'NextV1.2'
    header[9] = 1
    struct.pack_into('<HH', header, 12, 0xbf00, 0xc000)
    header[18 + 40] = 1
    header[139] = 40
    path.write_bytes(header + code + bytes(16384-len(code)))


def main():
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='inspeccy-emul-beam-') as tmp:
        root = Path(tmp)
        nex = root/'split.nex'
        fixture(nex)
        for hz in [50, 60]:
            for accurate in [False, True]:
                args = [str(exe), str(nex), '--vfs', str(root), '--hz', str(hz), '--bench', '8']
                if accurate:
                    args.append('--pixel-accurate')
                result = subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, result.stderr
                expected_log = 'pixel-accurate beam' if accurate else 'end-of-frame snapshot'
                assert expected_log in result.stderr
                ppm = (root/'inspeccy-emul-bench.ppm').read_bytes()
                header = b'P6\n640 256\n255\n'
                assert ppm.startswith(header), ppm[:32]
                assert len(ppm) == len(header) + 640 * 256 * 3
                def pixel(y):
                    at = len(header) + (y*640 + 320)*3
                    return tuple(ppm[at:at+3])
                top, bottom = pixel(80), pixel(176)
                if accurate:
                    assert top[1] > 200 and top[0] == top[2] == 0, (hz, top)
                    assert bottom[2] > 200 and bottom[0] == bottom[1] == 0, (hz, bottom)
                else:
                    assert top == bottom and top[0] > 200 and top[1] == top[2] == 0, (hz, top, bottom)
                print(f'PASS: {hz} Hz {expected_log}: upper {top}, lower {bottom}')


if __name__ == '__main__':
    main()
