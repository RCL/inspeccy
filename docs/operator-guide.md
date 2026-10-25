# inspeccy operator manual

inspeccy runs an input program to a requested budget or stop condition and writes inspection artifacts. The model and enabled features affect both correctness and execution cost. Use the same executable, input files, firmware, machine settings, and budget for reproducible comparisons.

Run `inspeccy --version` for the release number and `inspeccy --help` for the complete option list. The examples below use relative paths within a standalone source checkout; replace program and ROM filenames with your own files.

## Inputs and machine selection

Choose one primary source: --sna, --z80, --tap, --nex, --dot, --resume, or --trd with --trd-run. --dck loads a Timex cartridge and can accompany a tape.

SNA/Z80 headers determine the default classic machine. NEX and dot inputs select Next. Explicit --machine selects 48k, 128k, +2a, +3, pentagon, next50, next60, ts2068, tc2068, tc2048, zxevo, or zxuno. A machine name selects the implemented timing and peripheral model; it does not imply complete hardware emulation.

- --rom48 accepts a 16 KB BASIC ROM or the back half of a combined 32 KB 128K ROM.
- --rom128 accepts a 16 KB editor ROM or the front half of that combined image.
- --rom48128 sets both of the above from one combined image.
- --rom-plus3 selects a 64 KB four-ROM +2A/+3 image.
- --rom-trdos supplies the 16 KB TR-DOS ROM.
- --rom-zxevo and --rom-zxuno supply firmware for those models.

No firmware is bundled. Timex 2068 requires its HOME and EXROM images for ROM boot paths. A matching EXROM is located beside the supplied HOME image using the supported Timex ROM filenames.

NEX/dot programs that execute only RAM code can run without firmware. The fallback interrupt stub is sufficient for some such programs, but is not a replacement BASIC or NextZXOS ROM. Programs calling other ROM routines need suitable firmware.

TAP loading implements standard-speed blocks using the ROM loader path. It does not model arbitrary tape pulse formats or custom fast loaders. --tap-type-frame changes when automatic LOAD keystrokes begin.

TRD/SCL support is a read-only TR-DOS subset: standard raw TRD geometry or SCL file data, drive A, sector reads, basic seek/step commands, and ROM paging. Sector availability and seeks are simplified. It is not a general disk-controller timing model. --trdos auto enables paging when a disk or snapshot flag requires it; on/off/sna override that choice.

Timex DCK support handles implemented DOCK chunks and EXROM chunk zero; HOME replacement chunks are ignored. Cartridge compatibility should be tested with the specific firmware.

## Budgets, interrupts, and stops

Use --seconds N for emulated elapsed time, --cycles N for an absolute CPU-cycle budget, or --until-frame N for the frame-based convenience conversion. On resume, the requested duration is added to the saved run position. Instructions complete atomically, so an instruction can cross the requested cycle limit; the summary reports the overshoot.

--mhz selects the initial 3.5/7/14/28 MHz speed on Next/ZX-Uno. Other machines use their modeled nominal clock; ZX Evolution switches through its hardware registers. Runtime turbo changes rescale time-based deadlines. An explicit --cycles budget stays in CPU cycles.

--until-pc ADDR stops before executing at the requested PC. PP:ADDR also requires that physical 8 KB RAM page. Use explicit 0x prefixes for hexadecimal addresses on this option.

--frame-int overrides the interrupt period. --no-interrupts disables the frame source; it does not mean that every peripheral interrupt source is disabled. --int-window auto uses the machine's modeled pulse, latch retains a pending request, and an integer selects a pulse length. Interrupt-driven programs can behave very differently when the frame source is disabled.

--reset-trap on is the default: reaching address zero with ROM mapped stops the run. Turn it off when a reset into firmware is intentional. --lenient-undoc-ed treats otherwise rejected undocumented ED instructions as NOPs; it changes the interpretation of the input.

| Exit | Meaning |
|---|---|
| 0 | Cycle/time budget reached, or initial dump produced |
| 1 | Invalid or unsupported opcode |
| 2 | HALT with interrupts disabled |
| 3 | Input, parse, or output I/O failure |
| 4 | Invalid command-line arguments or incompatible configuration |
| 5 | RestartTrap at ROM address zero |
| 6 | Recognized deliberate DI; HALT stop |
| 7 | Memory invariant violation with --exit-on-violation |
| 8 | --until-pc target reached |
| 9 | Batch execution or assertion failed |

Outputs may still be produced when execution stops with a nonzero result. A summary/status describes the emulation result; a later output failure can make the process exit 3. Exit zero alone does not establish that a game or demo rendered correctly.

## Timing and hardware limits

Contention is enabled by default on models with implemented contended RAM. --contention off disables it. The current model uses the ULA phase pattern for covered accesses. The legacy --contention-x value is retained in metadata and for compatibility; it does not change that pattern.

On the Next the same switch covers the 28 MHz read wait state: at NR $07 = 3 the FPGA holds every memory read machine cycle (opcode fetch, operand, data, stack, DMA source) for one extra T-state; writes and I/O are not delayed, and 3.5/7/14 MHz have no wait states. So 28 MHz is not a clean 8x of 3.5 MHz: a `nop` costs 5 T, `ld a,(hl)` 9 T. --contention off removes the wait state. The Next core's ULA contention at 3.5 MHz is not modeled.

--cycle-exact on enables frame-locked video sampling and is the default. The name does not promise transistor-level accuracy for every device. --dispatch full forces the general CPU dispatcher for comparison with the automatically selected specialization.

Next supports tested subsets of Z80N, MMU paging, ULA/Layer 2, lores, tilemaps, sprites, Copper, DMA, AY/DAC, and interrupts. Unsupported control sequences and register combinations may behave differently from hardware.

For independent sprite anchors, attribute 2 supplies X/Y mirror and rotation in both four- and five-byte formats. Attribute 4 supplies the 4-bit pattern flag/half selection, X scale in bits 4:3, Y scale in bits 2:1, and Y bit 8 in bit 0. Port-upload progression and lockstep cursors are modeled separately. Relative/composite sprite support is partial: unified transforms, palette-offset rules, and 4-bit inheritance are not fully implemented. These limitations matter when validating chained sprites.

--ram-mb 1 or 2 selects effective Next RAM. The default is 2 MB. In the 1 MB mode, absent high pages read as open bus and discard writes.

ZX Evolution and ZX-Uno implement selected memory, turbo, video, and palette features. They are not complete replacements for the machines' firmware environments or all attached devices.

## Output names and file handling

By default a run writes a JSON state dump, BMP, and status JSON. A filename
supplied with `--out` is also the basename for those defaults. Classic 48K/128K/Pentagon snapshot runs can also write SNA output. NEX/dot and the other machine models do not have a complete SNA representation.

--out sets the SNA destination where supported and otherwise the basename for default artifacts. --dump-out, --bmp-out, and --status-out select explicit files. --no-dump, --no-bmp, and --no-status suppress those outputs.

The CLI rejects aliases between its declared inputs and outputs, including existing hard links, and rejects conflicting output destinations. This protects against common accidental overwrites. Existing unrelated output files can still be replaced. Do not concurrently rename inputs or output directories during a run.

Frame patterns for --video-out and --paper-out require exactly one integer conversion: d, i, u, x, X, or o, optionally with a width and l/ll modifier. For example, frames/frame_%06d.bmp. A literal percent is written as %%. Dynamic widths, string conversions, and multiple conversions are rejected; the maximum width is 1024 and expanded path length is bounded.

## Screenshots, video, audio, and metrics

--bmp-composite writes the composed visible display. --bmp-per-layer requests the diagnostic layer layout. --bmp-exact on uses the retained frame/replay path for the completed frame; --bmp-exact off draws from current state. These can differ when a program changes screen memory while the beam is drawing. --pixel-accurate enables the eager per-line path used for raster inspection. --video-out always implies --pixel-accurate.

~~~sh
inspeccy --nex tests/data/nex/trace_workload.nex --seconds 2 --video-out frames/frame_%06d.bmp --audio-out run.wav --no-bmp --no-status --no-dump
~~~

--video-fps selects capture frequency; otherwise the model selects the default. --audio-rate accepts 8000..192000 Hz, default 44100. --audio-stereo selects abc, acb, or mono AY panning. WAV rendering mixes the modeled AY, DAC, and beeper sources. The Next's four 8-bit DACs follow its port map (A: $1F/$F1/$3F, B: $0F/$F3, C: $4F/$F9, D: $5F, B+C: $B3, A+D: $DF/$FB, decoded on the low byte); A and B are heard on the left, C and D on the right. $DF is always live; the others need NR $08 bit 3. --specdrum-out is a legacy alias for --audio-out.

--psg-out writes sampled AY registers at --psg-rate 50 or 60. It is a register capture, not sampled audio; sub-frame writes can be lost. --ts enables the second AY and writes a companion basename.2.psg. --no-ay disables AY emulation.

--motion-out and --histogram-out write per-frame CSV measurements. --paper-out writes paper-area images, with --metrics-block controlling block averaging. These are comparison aids, not perceptual correctness scores.

tools/snapshot_periodic.py replays the original source for each timestamp, preserves its inspection dump, and mounts a requested VFS read-only. It stops on failure or early program termination. Existing selected outputs require --force; unrelated files are retained. The PowerShell wrapper calls the same Python implementation.

## JSON dump and resume

`--dump-out` writes inspectable JSON and `--resume` restores the serialized
state. This includes CPU and memory state, Next registers, Copper, DMA, CTC,
sprites, the CLI's one/two AY chips, tape and TR-DOS position, virtual file
handles, input-script progress and relevant execution options. Statistics
and counters continue. The [resume regression](../tests/python/test_resume_equivalence.py)
compares split and uninterrupted runs for the covered workloads.

UART FIFOs and live host-side UART processes are not serialized. Interrupt
diagnostic history starts afresh on each invocation. A dump is therefore
not a complete checkpoint for every possible peripheral configuration.
Keep external files and the emulator build unchanged when comparing resumes.

A frame or cycle budget given to a resumed run counts from the dump's saved
position. `--trace-from-cycles` is an absolute count. A different `--input`
script on the resumed command line replaces future input events; held keys
remain held until released. Reusing the same script continues its saved
progress.

Outputs continue as well. The PSG capture holds its whole song, the --audio-out WAV of the resumed run holds the samples after the first run's (the two files play on as one), --video-out frame numbers and --coverage-out marks carry on, and --motion-out keeps its history. tests/python/test_resume_equivalence.py checks all of this against straight runs.

External files are referenced, not stored: ROMs, the tape, the disk image, a .dck cartridge and the files under the --vfs folder must still be what they were. Their paths are resolved relative to the dump file, explicit CLI paths override them, and a ROM, tape, disk or script whose SHA-1 changed draws a warning. A dump written by an older build restores what it contains.

Dump input is bounded to 128 MB and validated for JSON syntax, numeric ranges, and nesting. Do not edit fields casually: individually valid values can still describe an inconsistent machine. The slot table (memory.base_slots) and the bus block record the mapping and state in effect, and win over what the registers alone would derive.

SNA saves preserve only the state that the format supports. A 48K SNA stores PC on the stack, so saving adjusts SP and writes the saved PC to RAM. A 128K SNA has an explicit PC/paging trailer. Neither format records the complete sound/video/peripheral state.

## Keyboard and joystick scripts

--input reads a UTF-8/ASCII text script containing one event per line. Lines use a decimal frame number, an action, and a key. # begins a comment.

~~~text
100 tap ENTER
150 press SPACE
160 release SPACE
200 tap 5 KEMP_FIRE
~~~

Actions and key names are case-insensitive. tap expands to press followed by release, using --input-tap-frames (default 2) unless a positive duration is given. Frame values must fit an unsigned 64-bit integer; tap durations must fit a positive signed 32-bit integer and release times must not overflow. Extra tokens, malformed numbers, NUL bytes, and scripts larger than 16 MB are rejected.

Keys include letters, NUM0..NUM9, ENTER, SPACE, CAPS, SYMBOL, editor composites such as LEFT/RIGHT/DELETE, and KEMP_LEFT/RIGHT/UP/DOWN/FIRE. Events are stably ordered by frame; events at the same frame retain file order. --input-log off suppresses input diagnostics.

## Virtual filesystem

--vfs DIR mounts host files for supported NextZXOS calls, with --vfs-drive selecting the drive and --dot-arg supplying a command tail. --vfs-readonly rejects guest writes.

Path resolution checks canonical containment within the mounted directory, including existing symlink/junction targets and new leaf paths. This is not an operating-system sandbox against concurrent filesystem changes. Mount only the intended input/output directory. Guest VFS writes are separate from the CLI artifact destinations.

## Traces and symbols

--trace or --trace-out writes readable execution records. --trace-out-bin writes a BTR file and, by default, a .fidx frame index. --no-trace-index disables the sidecar and --trace-index-out changes its path.

--trace-queue-mb sets the background writer's buffering capacity, from 1 to 1024 MB, default 16. It is not a retention ring or a maximum trace size. Long runs can produce large files.

--trace-from-cycles and --trace-from-seconds defer recording. A traced run executes exactly as an untraced one: the trace reads opcode bytes without the CPU's memory timing, so a fault found untraced can be replayed under --trace-out from the nearest dump. --trace-r and --trace-memptr include those register details; --trace-format verbose includes more state. --trace-decode reads BTR and emits text. Binary traces contain some annotations without direct native-text equivalents.

Text traces include cycles, frame, PC/physical-page address, mnemonic, and effects. --sld is repeatable and adds sjasmplus symbols. The command-line prologue can contain the paths and arguments supplied for that run: inspect generated traces before sharing them.

The format definitions are in shared/include/trace/. Use the supplied decoder and viewer rather than assuming every record is a fixed size. Call graphs and reconstructed memory are derived views, with the limitations described in the viewer README.

## Coverage, watchpoints, and invariants

--coverage-out writes a physical RAM byte map: bit 0 means read/fetched, bit 1 means executed. --coverage-reset-at-pc clears it at the first matching fetch, useful for excluding a depacker. An unobserved byte is unobserved in that workload; it is not necessarily dead code.

--watch-phys BANK:OFFSET reports writes to a physical 16 KB bank and offset. Bank is decimal; offset is hexadecimal by default or decimal with a d prefix. The reported CPU PC can already have advanced past the writing instruction.

Repeat --invariants FILE to load read-only ranges:

~~~text
# Inclusive physical 8 KB pages; page numbers decimal, offsets hexadecimal.
readonly8k = [10:0000-10:1fff]
readonly16k = [2:0000-2:00ff)
~~~

Square brackets include an endpoint; parentheses exclude it. Writes in a protected range produce violation events. --violations-out selects JSONL output, --max-violations limits reports, and --exit-on-violation requests exit 7. Without it, the rules observe writes rather than prevent them.

--optimize-out produces dynamic peephole suggestions, filtered by --optimize-profile and --optimize-min-ratio. An observed redundant instruction may matter on an unexecuted path or for flags, timing, interrupts, or self-modifying code. Review every suggested rewrite; the report does not prove a globally safe optimization.

## Comparing runs

Compare the process result, selected JSON fields, screen/audio outputs, and workload-specific assertions. Use fixed cycle budgets when instruction-count changes matter. Keep firmware and model settings identical.

tools/perf_benchmark.py records repeated whole-process durations, including loading and startup, with input/executable hashes. It reports actual measured columns. Its optional Copper comparison measures that particular workload and cannot establish that Copper or tracing has zero cost in general.

See the [test guide](../tests/README.md) for the regression suite and prerequisite handling. Preserve failure artifacts and inspect differences before updating references.

## Batches, diagnostics and UART peers

Use [batch experiments](batch.md) for ordered patches, bounded runs and
assertions. [Diagnostics](diagnostics.md) describes `--inspect-out`, device
queries and IRQ attribution. `--capabilities` reports these interfaces as JSON.

`--uart-pi loop` connects a loopback peer; `--uart-pi "COMMAND"` connects a
child process through stdin/stdout. `--uart-esp` selects the other channel.
Commands run through the host shell. Use `--realtime` when the peer expects
wall-clock pacing: an unrestricted headless run can otherwise outrun it.
`companions/nextpi-sup` supplies a configurable supervisor shim. See the
[UART test notes](../tests/README.md#next-uart) for the modeled registers and
current interrupt limitations.
