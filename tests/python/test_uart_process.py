"""Next UART bridged to a child process: the Z80 sends bytes through $133B, a
Python child on the far end of the pipe answers each byte incremented, and the
program checks what came back through $143B. Needs sjasmplus (else exit 77)."""
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parents[1]
CHILD = r'''
import sys
data = sys.stdin.buffer
out = sys.stdout.buffer
while True:
    b = data.read(1)
    if not b:
        break
    out.write(bytes([(b[0] + 1) & 255]))
    out.flush()
    if b[0] == 0xFE:
        break
'''
PROGRAM = '''
	include "common.inc"
	ld bc, $153b
	ld a, $40
	out (c), a
	ld bc, $133b
	ld e, 0
.send	ld a, e
	out (c), a
	inc e
	ld a, e
	cp 16
	jr nz, .send
	ld a, $fe		; tells the child to stop after answering
	out (c), a
	ld hl, received
	ld e, 17
.wait	ld bc, $133b
	in a, (c)
	and $01
	jr z, .wait
	ld bc, $143b
	in a, (c)
	ld (hl), a
	inc hl
	dec e
	jr nz, .wait
	ld hl, received
	ld e, 1
.verify	ld a, (hl)
	cp e
	jr nz, .bad
	inc hl
	inc e
	ld a, e
	cp 17
	jr nz, .verify
	ld a, (hl)
	cp $ff
	jr nz, .bad
	pass
.bad	fail
received ds 17
'''


def main():
    if len(sys.argv) < 2:
        print("usage: test_uart_process.py <inspeccy>")
        return 2
    inspeccy = pathlib.Path(sys.argv[1])
    sjasmplus = shutil.which("sjasmplus")
    if not sjasmplus:
        print("sjasmplus not on PATH; skipping")
        return 77
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "child.py").write_text(CHILD)
        (tmp / "prog.asm").write_text(PROGRAM)
        subprocess.run([sjasmplus, f"-i{HERE / 'asm'}", f"--raw={tmp / 'prog.dot'}", str(tmp / "prog.asm")],
                       check=True, capture_output=True)
        cmd = [str(inspeccy), "--dot", str(tmp / "prog.dot"), "--machine", "next50", "--seconds", "1",
               "--no-status", "--no-dump", "--no-bmp", "--uart-pi", f"{sys.executable} {tmp / 'child.py'}"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 5:  # pass macro -> restart trap
            print("expected exit 5 (pass), got", r.returncode)
            print(r.stdout[-2000:])
            print(r.stderr[-2000:])
            return 1
        if "uart1: 17 bytes out, 17 in" not in r.stderr:
            print("unexpected link statistics:", r.stderr)
            return 1
    print("uart process bridge: 17 bytes each way through a child process")
    return 0


if __name__ == "__main__":
    sys.exit(main())
