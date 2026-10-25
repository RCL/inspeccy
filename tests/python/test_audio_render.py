#!/usr/bin/env python3
"""Unified audio render (--audio-out) acceptance.

Synthesises 48K SNAs that program the AY directly (Melodik-style $FFFD/$BFFD
on a 48K machine) and checks the rendered WAV:

  1. TONE:   R0/R1 = period 252 (~434 Hz at the 48K's 1.75 MHz AY clock), tone A only.
             Measured frequency (mean-crossing count) must be within 1%.
  2. NOISE:  noise A only -> broadband; no dominant period (crossing count
             far from any pure tone's, and RMS well above silence).
  3. ENVELOPE RETRIGGER: volume in envelope mode, shape 0 (single decay),
             R13 written twice ~0.5 s apart WITH THE SAME VALUE -> two
             distinct decaying bursts (retrigger must not be deduped).
  4. DETERMINISM: the tone render, run twice, is byte-identical (SHA1).
  5. BEEPER: a 48K program toggling $FE bit 4 in a timed loop renders a
             square tone at the loop's frequency (~883 Hz) within 1%.
Exit 0 = pass, 1 = fail, 77 = prerequisite (ROM) missing.
"""

from __future__ import annotations

import hashlib
import struct
import subprocess
import sys
import wave
from pathlib import Path

from test_support import binary_candidates, locate_test_binary, test_rom

TESTS_DIR = Path(__file__).resolve().parents[1]
INSPECCY_DIR = TESTS_DIR.parent
ROM48 = test_rom("48.rom")



def locate_inspeccy():
    return locate_test_binary()


def ay_write_code(reg: int, val: int) -> bytes:
    """OUT ($FFFD),reg then OUT ($BFFD),val via BC."""
    return bytes([
        0x01, 0xFD, 0xFF,        # ld bc, $FFFD
        0x3E, reg,               # ld a, reg
        0xED, 0x79,              # out (c), a
        0x01, 0xFD, 0xBF,        # ld bc, $BFFD
        0x3E, val,               # ld a, val
        0xED, 0x79,              # out (c), a
    ])


def build_sna(prog: bytes) -> bytes:
    ram = bytearray(49152)
    ram[0x8000 - 0x4000 : 0x8000 - 0x4000 + len(prog)] = prog
    ram[0xFFFE - 0x4000 : 0xFFFE - 0x4000 + 2] = bytes([0x00, 0x80])   # stacked PC
    header = struct.pack(
        "<B HHHH HHH HH B B H H B B",
        0x3F, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x00,            # IFF2 clear = DI
        0x00, 0x0000,
        0xFFFE,          # SP
        1, 0,
    )
    return header + bytes(ram)


SPIN = bytes([0x18, 0xFE])   # jr $


def delay_loop(iters: int) -> bytes:
    """~26-cycle countdown loop: ld bc,N / dec bc / ld a,b / or c / jr nz."""
    return bytes([
        0x01, iters & 0xFF, (iters >> 8) & 0xFF,   # ld bc, N
        0x0B,                                       # dec bc
        0x78,                                       # ld a, b
        0xB1,                                       # or c
        0x20, 0xFB,                                 # jr nz, -5
    ])


def run(binp: Path, sna: Path, wav: Path, seconds: float) -> None:
    r = subprocess.run(
        [str(binp), "--sna", str(sna), "--machine", "48k",
         "--rom48", str(ROM48), "--seconds", str(seconds),
         "--audio-out", str(wav), "--audio-stereo", "mono",
         "--no-dump", "--no-bmp", "--no-status",
         "--out", str(TESTS_DIR / "tmp" / "audio.out.sna")],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL: inspeccy exit {r.returncode}\n{r.stderr[-400:]}")
        sys.exit(1)


def load_wav(path: Path):
    w = wave.open(str(path))
    n = w.getnframes()
    rate = w.getframerate()
    data = struct.unpack(f"<{n * w.getnchannels()}h", w.readframes(n))
    return list(data), rate


def mean_crossings(samples, rate) -> float:
    mean = sum(samples) / len(samples)
    c = 0
    prev = samples[0] - mean
    for s in samples[1:]:
        v = s - mean
        if (v > 0) != (prev > 0):
            c += 1
        prev = v
    return c / 2.0 / (len(samples) / rate)   # full periods per second


def rms(samples) -> float:
    mean = sum(samples) / len(samples)
    return (sum((s - mean) ** 2 for s in samples) / len(samples)) ** 0.5


def main() -> None:
    binp = locate_inspeccy()
    tmp = TESTS_DIR / "tmp"
    tmp.mkdir(exist_ok=True)
    ok = True

    # ---- 1. tone: period 252 -> 1750000 / (16 * 252) = 434.03 Hz
    #      (48k machine: AY clock = nominal CPU / 2 = 1.75 MHz, Melodik-style)
    tone = (ay_write_code(7, 0x3E)          # mixer: tone A only
            + ay_write_code(0, 252 & 0xFF)  # R0
            + ay_write_code(1, 252 >> 8)    # R1
            + ay_write_code(8, 15)          # full fixed volume
            + SPIN)
    sna = tmp / "audio_tone.sna"
    sna.write_bytes(build_sna(tone))
    wav = tmp / "audio_tone.wav"
    run(binp, sna, wav, 1.0)
    samples, rate = load_wav(wav)
    # Skip the first 0.1 s (program setup) for the measurement.
    meas = samples[rate // 10:]
    freq = mean_crossings(meas, rate)
    expected = 1750000.0 / (16 * 252)
    print(f"tone: measured {freq:.2f} Hz, expected {expected:.2f} Hz")
    if abs(freq - expected) > expected * 0.01:
        print("FAIL: tone frequency off by more than 1%")
        ok = False
    if rms(meas) < 500:
        print(f"FAIL: tone too quiet (rms={rms(meas):.0f})")
        ok = False

    # ---- 4. determinism: identical render on a second run
    wav2 = tmp / "audio_tone2.wav"
    run(binp, sna, wav2, 1.0)
    h1 = hashlib.sha1(wav.read_bytes()).hexdigest()
    h2 = hashlib.sha1(wav2.read_bytes()).hexdigest()
    if h1 != h2:
        print(f"FAIL: render not deterministic ({h1} vs {h2})")
        ok = False
    else:
        print(f"determinism: byte-identical renders ({h1[:12]})")

    # ---- 2. noise: noise A only, no tone
    noise = (ay_write_code(7, 0x37)         # mixer: noise A only
             + ay_write_code(6, 5)          # noise period
             + ay_write_code(8, 15)
             + SPIN)
    sna_n = tmp / "audio_noise.sna"
    sna_n.write_bytes(build_sna(noise))
    wav_n = tmp / "audio_noise.wav"
    run(binp, sna_n, wav_n, 1.0)
    ns, nrate = load_wav(wav_n)
    nmeas = ns[nrate // 10:]
    nfreq = mean_crossings(nmeas, nrate)
    nrms = rms(nmeas)
    print(f"noise: crossings-rate {nfreq:.0f}/s, rms {nrms:.0f}")
    if nrms < 500:
        print("FAIL: noise too quiet")
        ok = False
    # Broadband: mean-crossing rate far above any musical tone.
    if nfreq < 2000:
        print("FAIL: noise looks periodic, not broadband")
        ok = False

    # ---- 3. envelope retrigger: shape 0 decay, R13 written twice (same value)
    #      env period 2000 -> full decay = 16 * 16 * 2000 / 1773450 = 0.289 s
    delay_half_s = delay_loop(0xFFFF) + delay_loop(0x0A50)   # ~ 26 * (65535 + 2640) = 1.77M T = 0.51 s
    env = (ay_write_code(7, 0x3E)
           + ay_write_code(0, 252 & 0xFF) + ay_write_code(1, 252 >> 8)
           + ay_write_code(11, 2000 & 0xFF) + ay_write_code(12, 2000 >> 8)
           + ay_write_code(8, 0x10)         # envelope mode
           + ay_write_code(13, 0x00)        # shape 0: single decay -> silence
           + delay_half_s
           + ay_write_code(13, 0x00)        # SAME value: must retrigger
           + SPIN)
    sna_e = tmp / "audio_env.sna"
    sna_e.write_bytes(build_sna(env))
    wav_e = tmp / "audio_env.wav"
    run(binp, sna_e, wav_e, 1.2)
    es, erate = load_wav(wav_e)
    # RMS in 50 ms windows; count distinct loud bursts.
    win = erate // 20
    levels = [rms(es[i:i + win]) for i in range(0, len(es) - win, win)]
    thresh = max(levels) * 0.3
    bursts = 0
    above = False
    for L in levels:
        if L > thresh and not above:
            bursts += 1
            above = True
        elif L < thresh * 0.5:
            above = False
    print(f"envelope: {bursts} bursts (windows above {thresh:.0f})")
    if bursts != 2:
        print("FAIL: expected exactly 2 bursts from 2 identical R13 writes")
        ok = False

    # ---- 5. beeper: toggle $FE bit 4 in a timed loop
    #      half period = 7 (xor) + 11 (out) + 7 (ld b) + 13*(N-1)+8 (djnz) + 12 (jr)
    #                  = 32 + 13*N cycles; N=150 -> 1982 -> 3.5e6/3964 = 883.0 Hz
    N = 150
    beep = bytes([
        0xAF,                    # xor a  (start with bit4=0, border 0)
        0xEE, 0x10,              # loop: xor $10
        0xD3, 0xFE,              # out ($FE), a
        0x06, N,                 # ld b, N
        0x10, 0xFE,              # djnz $
        0x18, 0xF6,              # jr loop (back 10: to the xor)
    ])
    sna_b = tmp / "audio_beep.sna"
    sna_b.write_bytes(build_sna(beep))
    wav_b = tmp / "audio_beep.wav"
    run(binp, sna_b, wav_b, 1.0)
    bs, brate = load_wav(wav_b)
    bmeas = bs[brate // 10:]
    bfreq = mean_crossings(bmeas, brate)
    bexpected = 3500000.0 / (2 * (32 + 13 * N))
    print(f"beeper: measured {bfreq:.2f} Hz, expected {bexpected:.2f} Hz")
    if abs(bfreq - bexpected) > bexpected * 0.01:
        print("FAIL: beeper frequency off by more than 1%")
        ok = False
    if rms(bmeas) < 500:
        print(f"FAIL: beeper too quiet (rms={rms(bmeas):.0f})")
        ok = False

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
