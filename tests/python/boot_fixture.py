"""Small original BASIC/machine-code fixtures for tape and disk boot tests."""
from pathlib import Path
import struct


def payload():
    # DI; write $55AA at $9000; fill pixels with $FF and attributes with $42.
    code = bytes.fromhex('f3 21 aa 55 22 00 90 21 00 40 11 01 40 01 ff 17 36 ff ed b0 '
                         '21 00 58 11 01 58 01 ff 02 36 42 ed b0')
    # Repeated AY channel-A volume writes. Relative branches allow the REM
    # payload to execute at its BASIC address without relocation.
    loop = bytes.fromhex('01 fd ff 3e 08 ed 79 01 fd bf 3e 0f ed 79')
    return code + loop + bytes((0x18, (-len(loop) - 2) & 255))


def basic_program():
    def line(number, body):
        body += b'\r'
        return struct.pack('>H', number) + struct.pack('<H', len(body)) + body
    # Read PROG from the system variables: TR-DOS reserves extra workspace.
    rem = line(1, b'\xea' + payload())
    def number(value):
        return str(value).encode('ascii') + b'\x0e\0\0' + struct.pack('<H', value) + b'\0'
    call = (b'\xf9\xc0(\xbe' + number(23635) + b'+' + number(256)
            + b'*\xbe' + number(23636) + b'+' + number(5) + b')')
    return rem + line(10, call)


def write_tap(path: Path):
    def block(flag, data):
        body = bytes((flag,)) + data
        checksum = 0
        for value in body:
            checksum ^= value
        body += bytes((checksum,))
        return struct.pack('<H', len(body)) + body
    basic = basic_program()
    header = b'\0' + b'boot_test ' + struct.pack('<HHH', len(basic), 10, len(basic))
    path.write_bytes(block(0, header) + block(255, basic))


def write_trd(path: Path, name: str):
    basic = basic_program()
    data = basic + bytes.fromhex('80 aa 0a 00')
    sectors = (len(data) + 255) // 256
    image = bytearray(655360)
    # Directory: BASIC length, variables offset, sectors, sector and track.
    image[:16] = name.encode('ascii').ljust(8, b' ') + b'B' + struct.pack('<HHBBB', len(basic), len(basic), sectors, 0, 1)
    image[0x1000:0x1000 + len(data)] = data
    service = 0x800
    image[service + 0xE1:service + 0xE5] = bytes((sectors, 1, 0x16, 1))
    struct.pack_into('<H', image, service + 0xE5, 2544 - sectors)
    image[service + 0xE7] = 0x10
    image[service + 0xF5:service + 0xFD] = b'BOOTTEST'
    path.write_bytes(image)


def check_run(result, sna: Path):
    import re
    assert result.returncode == 0, f'boot failed ({result.returncode}): {result.stderr}'
    data = sna.read_bytes()
    assert data[27 + 0x5000:27 + 0x5002] == b'\xaa\x55', 'payload did not write its RAM signature'
    assert data[27:27 + 6144] == b'\xff' * 6144, 'pixel fill missing'
    assert data[27 + 6144:27 + 6912] == b'\x42' * 768, 'attribute fill missing'
    count = re.search(r'aywrites=(\d+)', result.stdout)
    assert count and int(count[1]) > 1000, 'AY output loop did not execute'
    return int(count[1])
