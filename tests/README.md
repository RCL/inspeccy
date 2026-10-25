# inspeccy tests

The suite combines small synthetic programs, captured regression references, and independent assertions about selected output behavior.

## Running

From the source directory:

~~~sh
ctest --test-dir build -C Release --output-on-failure
python tests/run_tests.py --inspeccy build/inspeccy/inspeccy
python tests/run_tests.py --inspeccy build/inspeccy/inspeccy --rom-dir roms --require-no-skips
python tests/run_tests.py --filter sprites --verbose
~~~

With Visual Studio, the executable is build/inspeccy/Release/inspeccy.exe. The runner can also locate common local build paths. Requirements are Python 3.10 or newer and sjasmplus on PATH. Some standalone tests need additional Python packages; their diagnostics identify them.

ROMs are external inputs. Supply --rom-dir or INSPECCY_ROM_DIR. The fallback is `roms/` under the source directory. Common images are 128.rom, trdos504t.rom, plus3.rom, and the Timex ROM pairs required by their tests. Tests use these images only when needed; assembly and RAM-only cases do not require firmware.

Standalone tests return 77 when a prerequisite is unavailable. The runner reports these as skips; --require-no-skips fails a run containing any. Nonzero results other than 77 are failures. Case files can explicitly declare an expected failure; standalone test entries do not support that convention.

## Test groups

`test_batch.py` tests the public `--batch` JSON interface with a generated NEX,
without ROMs or an assembler. It covers repeated calls, physical RAM patches,
bank-qualified breakpoints, dump/resume continuation, compact observations,
assertion/timeout failures, validation before mutation and output path safety.
It is also registered as CTest `batch_cli`. Run it directly with:

```sh
python tests/python/test_batch.py build/inspeccy/Release/inspeccy.exe
```

For new tests with known sequences, batch the patches and checks so the state
is loaded once and written once, rather than once per routine call. Keep
independent cases in separate invocations. See the [batch format and timing
explanation](../docs/batch.md).

- CTest: shared parsers and frame filenames, independent sprite pixel/cursor assertions, VFS mount boundaries, and CLI validation when Python and tracing are enabled.
- cases/: declarative end-to-end tests using synthetic snapshots, cartridges, input scripts, and NEX files.
- asm/: small assembly tests of instructions, paging, video, and audio. A nonzero assembler exit or assembler warning fails the test.
- python/test_*.py: trace round trips, generated tape/disk boot fixtures, rendering, timing, malformed inputs, resume equivalence (split runs against straight ones), dispatch comparisons, and CPU fingerprints.
- refs/: BMP references for the assembly compositor tests.
- cpp/: native unit tests registered with CTest.
- data/: fixtures, sample programs and their documented provenance.
- tmp/: ignored generated outputs and failure diagnostics; not release inputs.

The full standalone list is in run_tests.py. A successful suite validates the covered cases. CPU fingerprint data was captured from an earlier emulator build and detects regressions; it is not an independent CPU oracle. BMP references similarly require human review or independent pixel assertions when rendering behavior changes.

`block_io_flags` runs all eight block I/O opcodes through the CPU with an
independent flag oracle, on classic and Next plain/full dispatch paths. It covers
H/C and parity arithmetic, N from the transferred byte, C/L and B wraparound,
16/21-T-state timing, MEMPTR, output-port B ordering, complete repeat loops, and
flags exposed to an interrupt between iterations. `outd_flags` retains the
end-to-end CLI regression for an AY output loop.

## Assembly tests

The runner assembles each .asm file under asm/ except names beginning with an underscore. Include common.inc for device setup and lowercase assertion macros:

~~~asm
	include "common.inc"

	ld a, $12
	db $ed, 36
	check_a $48
	pass
~~~

The pass macro jumps to address zero and expects RestartTrap (exit 5). A failed assertion executes DI; NOP; HALT and exits 2. Video tests compare the snapshot compositor against refs/video/ and explicitly use --bmp-exact off. Frame-replay rendering is tested separately.

A source comment containing @needs-audio-out requests WAV output. @ram-mb 1 selects the smaller Next RAM configuration for an individual test. Keep these runner markers intact.

Use --update-refs only after establishing why the changed image is correct. The option overwrites the selected BMP references; it is not a way to dismiss a regression.

The 2026-10-04 transparency correction replaces exposed white/green background
pixels in 38 video references with the reset black fallback. Opaque pixels and
ordinary outer borders are unchanged. Register `$4A=0` is a valid black colour,
not an unset value; ULANext full-ink mode (`$42=255`) also uses `$4A` for its border.
See the [hardware register definition](https://wiki.specnext.dev/Transparency_colour_fallback_Register)
and `test_transparency_fallback.cpp` for independent pixel assertions covering
zero/nonzero colours, narrow/wide Layer 2, LoRes, full-ink borders, and beam versus
snapshot output. Native/WASM agreement alone cannot catch a shared renderer bug.

## Next UART

`core/src/uart.h` models the Next's two-channel UART from the FPGA sources:
the four ports, channel select, 17-bit prescaler, framing register, 512-byte
Rx and 64-byte Tx FIFOs, the status bits and their clear-on-read, and byte
timing at the programmed baud against the 28 MHz system clock. NR $A0 bit 4
gates the Pi channel as on hardware (a cold boot with `--uart-pi` sets it).
The far end of a channel is a *backend*: `loop` echoes, anything else is run
through the shell with its stdin/stdout as the wire. The Rx interrupt sources
of NR $C6 are raised under IM2 hardware vectoring (NR $C0 bit 0): "Rx
available" or "Rx near full" (384 of 512 queued), source index 1 (UART0) and 2
(UART1), with the NR $CA status bits (write 1 to clear). The FIFO is sampled on
a 100 us grid for the request level, so a program that stops polling sees it
rise on time. Tx-empty interrupts are not raised. `INSPECCY_UART_DEBUG=1` makes
`inspeccy-emul` print the CPU state at each byte the Rx FIFO drops. UART state
is not part of JSON dumps.

An asm test opts in with `; @uart-pi loop`. `python/test_uart_process.py`
bridges a small Python child process and checks the bytes received. It needs
sjasmplus; no external Pi project is required. Both frontends accept the same
UART options. Use `--realtime` when a peer expects wall-clock pacing;
`INSPECCY_PACE_DEBUG=1` reports emulated and elapsed host time.

`inspeccy-emul` draws its frame 640 pixels wide (`RenderCompositeBgrHires`: the
640x256 Layer 2 mode keeps both pixels of every byte, every other layer is
doubled); the headless `--bmp-out` composite stays 320 wide, as the test
oracles expect.

`companions/nextpi-sup` is a stand-in for the NextPi supervisor to put on the
far end: `--uart-pi "nextpi-sup --root DIR --native '/ram/prog=HOST [args]' --log FILE"`
(words after the host path go before the guest's own arguments). It prints
`ready` on stderr when it starts; `inspeccy-emul` warns when the far-end
process is not running. `inspeccy-emul --input SCRIPT` plays a keyboard script
on top of the live keyboard.
It answers Enter with `SUP> `, echoes like the serial console, takes the PIPUT
file transfer into `DIR/ram`, runs a mapped native binary in place of the ARMv6
one with the wire as its stdin/stdout, and logs every byte. Its replies are
reconstructed from the public NextPi documents and the PiTools client, not from
the Pi scripts. Treat it as a development shim, not a complete NextPi system.

## Case files

Each cases/*.test contains key = value lines, with # comments. Paths to source artifacts are relative to tests/. A minimal case is:

~~~text
description = synthetic program reaches the cycle limit
sna = data/sna/nop_loop.sna
cycles = 1000
expect_exit = 0
~~~

Use an existing fixture name or add the corresponding generated fixture. Select sna, dot, nex, dck, or a supported two-phase case, and specify a cycle/time limit unless create_initial_dump is true. Optional assertions cover summary text, trace patterns, JSON fields, hashes, and output properties. The parser and command construction in run_tests.py define the accepted keys; existing cases demonstrate each supported form.

## Fixture provenance

See [data/README.md](data/README.md) for the fixture inventory and sources.
The small SNA instruction fixtures can be regenerated and checked on any
supported Python platform:

```sh
python tests/data/sna/src/generate.py --check
```

Firmware is supplied separately through `--rom-dir` or `INSPECCY_ROM_DIR`.
The optional external effects-header check needs `INSPECCY_EFFECTS_HEADER`;
without it that check reports a skip. `--require-no-skips` is useful when all
selected prerequisites have deliberately been supplied.

`test_tooling.py <inspeccy>` / CTest `shared_tooling` exercises shared snapshot
editing, relocated file references, immutable originating-build identity,
manifest/SLD mismatch rejection, typed symbolic batches and failure details,
layout reports and bank contracts. It needs no game, ROM or assembler. Run it
in tracing-disabled builds too: symbol lookup is independent of trace support.

## Diagnostics and shared agent skills

`inspection_core` and `inspection_cli` test stopped-state mapping/annotations,
raw and RGB333 device reads, guest I/O uploads, observation side effects, bounded
reports, rejected plans and frame/line interrupt attribution. They run with
tracing on or off and generate their own inputs without ROM/game dependencies.
`agent_skills` checks both client manifests, shared references and safe installer
updates. It does not call a model or claim behavioural parity between agents.

For local timing evidence, run `python tests/python/benchmark_diagnostics.py PATH_TO_EXE`
with optional `--baseline OLD_EXE` and `--out result.json`. It alternates repeated
runs of branch/memory workloads and compares compact device output with a dump.
Use controlled local measurements rather than an absolute CI speed threshold.
