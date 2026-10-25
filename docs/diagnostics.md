# Compact native diagnostics

These C++ CLI interfaces require no Python, model, plugin, ROM or persistent
server. `inspeccy --capabilities` prints versioned JSON listing supported batch
operations, devices, diagnostic schemas, limits and whether tracing was compiled
in. `capability_revision` identifies the interface revision, not a game build.
All features here also work with `INSPECCY_TRACE_SUPPORT=OFF`.

## Stopped CPU, mapping and stack

```text
inspeccy --resume seed.json --build-manifest program.build.json --cycles 100000 --inspect-out stopped.json --no-dump --no-bmp --no-status
```

Or add a batch step:

```json
{"op":"read_state","name":"boundary","stack_words":16,"pc_bytes":16}
```

Both counts default to 16, allow zero and are capped at 64. `read_state` returns
`observations[].state`; the standalone report has `version`, `stop`, `batch_exit`
and `state`. Batch execution/assertion failures include a smaller `failure.state`
automatically. Invalid plans are rejected before mutation, without an execution
failure context. `--inspect-out` can still show the initial state after a bad plan.

State contains CPU/alternate registers, cycles, PC mapping and nearby labels,
raw PC bytes, bounded forward disassembly, all eight effective read/write slot
mappings, requested Next MMU registers, and a stack slice. Bytes use the actual
read mapping, including Layer 2 overlays; read and write mappings may differ.
RAM, ROM, open bus, other and unavailable backing are distinguished. CPU addresses
wrap at 16 bits and the report marks wrapped slices. An incomplete final opcode
is not disassembled.

Labels are bounded nearest physical-offset annotations (within 256 bytes), so a
RAM page mapped into another CPU window still resolves. Up to four aliases are
reported, with omitted/ambiguous flags and bounded name text. ROM/open bus is
never labelled using unrelated RAM symbols. The report's identity states whether
symbols are manifest-bound, unverified or used with an explicit mismatch override.
The original NEX hash and override reason are retained.

Stack words are **unclassified**, with candidate address annotations using
current mappings. This is not an unwinder: saved registers, data and old bank
mappings cannot be inferred from the word value alone. Debug inspection changes
no guest state, device cursors, coverage or timing.

## Device reads and assertions

```json
{"op":"read_device","device":"palette","layer":"layer2","bank":1,"start":0,"count":16}
{"op":"expect_device","device":"palette","layer":"layer2","bank":1,"start":0,"values":[0,511]}
{"op":"read_device","device":"sprite_attrs","start":0,"count":8}
{"op":"read_device","device":"sprite_pattern","slot":3,"offset":0,"length":32}
{"op":"expect_device","device":"ay","chip":0,"start":7,"values":[56]}
```

Reads return `device_state` inside the batch observation. Expectations produce
no successful observation; a failure includes `device_mismatch` with selector,
relative byte/entry index, unit, expected and actual value. An absent device is
an error; it never yields invented zeros. Selectors are validated with the rest
of the plan before any step executes.

| Device | Selection and encoding |
| --- | --- |
| `palette` | `layer`: ula/layer2/sprites/tilemap; `bank`: 1/2; `start`/`count`: entries within 0..255. Default `encoding:"rgb333"` gives integers in `values`. `encoding:"raw"` gives two bytes per entry in `hex`. Next only. |
| `sprite_attrs` | `start`/`count` within 128 slots; `hex` contains five stored bytes per slot, even for a four-byte sprite. |
| `sprite_pattern` | `slot` 0..63; `offset`/`length` within its 256 bytes; result is `hex`. |
| `ay` | `chip` 0/1; `start`/`count` within 16 registers; result is `values`, `available`, global emulation `enabled`, `latch` and `active_chip`. Chip 1 requires `--ts`. |

`expect_device` infers length from `values` (RGB333/AY) or `hex` (raw palettes/
sprites), so omit count/length. Raw palette expectations must contain whole
two-byte entries and attributes whole five-byte records. Unknown fields and
mixed expectation encodings are errors. Sprite reads also return upload counts
and both cursors, useful for detecting unexpected writes even if bytes match.

Palette IDs in old snapshots follow NR43 bits 6:4: ULA-1, L2-1, Sprites-1,
Tilemap-1, ULA-2, L2-2, Sprites-2, Tilemap-2. Existing `pal_N` IDs have not changed.
RGB333 decoding is `(byte0 << 1) | (byte1 & 1)`; raw mode preserves other bits.
AY registers are stored values after hardware masks. On Next, guest code also
needs NR08 bit 1 to switch TurboSound chips; the global `enabled` field is not
that hardware gate.

Reads use const device state, never emulated port reads. The batch observation
budget remains 16 MiB, conservatively including escaped labels and failure
context. Request dumps only when a later experiment needs to resume/branch.

## Interrupt attribution

Configuration version 1:

```json
{"version":1,"sources":["frame","line"],"max_events":256,"guards":[
  {"name":"update_busy","source":"frame","symbol":"UpdateGuard","type":"u8","idle_value":0}
]}
```

```text
inspeccy --nex program.nex --build-manifest program.build.json --cycles 1000000 --irq-diagnostics irq.json --irq-diagnostics-out events.json --no-dump --no-bmp --no-status
```

The default output is CONFIG.result.json. Both sources default on; `guards` is
optional. Up to 16 guards describe manifest-bound physical symbols or explicit
`page`/`offset`, a `u8`/`u16le` type and idle value. Address-only EQU symbols need
explicit physical coordinates. Guard reads cannot cross 8K pages.

The report distinguishes `frame_pulse_expired`, `line_pulse_expired` and
`guard_busy_at_accept`. A masked pulse is suppressed; latched interrupt sources
do not expire. A busy guard means its value was non-idle before CPU acceptance;
it does not prove the guest handler subsequently skipped an update.

Each sample carries pre-accept/expiry PC and SP mappings, IFF/IM state, cycles,
turbo-aware emulated nanoseconds, batch step, and guard value or pulse deadline.
PC is the location where the instruction-boundary dispatcher observed the event,
not proof of the exact instruction that crossed a deadline. DMA/contention can
also delay observation. No symbols, strings or JSON are processed in the CPU
loop; stored numeric locations are annotated after execution.

The default keeps the first 256 events, with a configurable maximum of 4096;
zero retains counts only. Up to 256 physical-location buckets are sorted by
count. `omitted_events` and `location_overflow_events` report discarded detail.
Counts cover all runs within one invocation. Resuming a snapshot starts a new
capture, identified by `initial_cycle`; diagnostic history is not snapshot state.
Status and batch reports include compact totals. Capture is disabled during
pixel replay and introduces no new stop reason or interrupt servicing changes.

## Validation and skills

`tests/cpp/test_inspection.cpp` and `test_inspection.py` cover mapping, device uploads,
side effects, invalid plans, limits, pulse/guard distinctions, 50/60 Hz and turbo
changes. `benchmark_diagnostics.py` compares local repeated timings and report
sizes, optionally against `--baseline` from before a change; it is not a portable
CI timing threshold. Compare interrupt observations with the program's own
counters when establishing what a missed update means.

The [Inspeccy Skills package](../plugins/inspeccy-skills/README.md) provides shared
Codex/Claude skills and an installer. Skills explain how to choose an experiment;
these native interfaces retain responsibility for execution and validation.
