"""Guest reads appended NEX data using the loader-supplied BC file handle."""
import pathlib
import subprocess
import sys
import tempfile


def main():
    with tempfile.TemporaryDirectory(prefix="nex-bc-") as temp:
        root = pathlib.Path(temp)
        header = bytearray(512)
        header[:8] = b"NextV1.2"
        header[9] = 1
        header[12:16] = bytes((0, 0xBF, 0, 0x80))
        header[18 + 2] = 1
        header[140] = 1
        # DI; LD A,C; IX=HL=$8100; BC=1; F_READ; reject carry or wrong byte.
        code = bytearray.fromhex("F3 79 DD 21 00 81 21 00 81 01 01 00 CF 9D")
        code += bytes((0xDA, 0x1C, 0x80))  # JP C,fail
        code += bytes.fromhex("3A 00 81 FE D9 C2 1C 80 C3 00 00 F3 00 76")
        bank = code + bytes(16384 - len(code))
        nex = root / "bc.nex"
        nex.write_bytes(header + bank + b"\xD9")
        result = subprocess.run([sys.argv[1], "--machine", "next50", "--nex", str(nex),
                                 "--vfs", str(root), "--vfs-readonly", "--cycles", "100000",
                                 "--until-pc", "0x8019", "--no-dump", "--no-bmp", "--out", str(root / "out")],
                                capture_output=True, text=True)
        if result.returncode != 8:
            print(result.stdout, result.stderr)
            return 1
        print("BC NEX handle read appended byte successfully")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
