# inspeccy command-line experiments

The CLI supports finite JSON batches through `--batch`. This interface is
ordinary command-line input, available to any model, shell or test framework.
It does not require MCP, a Python library or a persistent server.

## Why batching saves time

A dump/resume experiment normally loads a snapshot, edits a few bytes, writes
the snapshot again, launches the emulator, and reads another full snapshot.
Doing that for every small check repeats the expensive work. A batch loads
the starting state once, applies small changes directly to the stopped
machine, runs the requested work, and writes its results once at the end.

This retains a replayable artifact: the source NEX or dump plus the JSON plan.
Use separate invocations when the next experiment depends on inspecting the
previous result; put already-known sequences and repetitions in a batch.
Large RAM dumps can stay on disk. Models usually only need the small result
JSON containing explicitly requested observations and any failed check.

## Invocation

```sh
inspeccy --resume ready.inspec.dump.json --batch experiment.json \
  --batch-out result.json --dump-out final.inspec.dump.json --no-bmp --no-status
```

`--nex`, `--sna` and the other normal load sources also work. Their existing
ROM and machine requirements apply. Omit `--dump-out` and use `--no-dump`
when only the compact report is needed. Without `--batch-out`, the report
is written to `<batch filename>.result.json`. Existing final dump, screenshot,
trace and other output flags retain their normal meaning; there are no
automatic intermediate snapshots. Input scripts and VFS flags still apply.

Batch runs use per-operation cycle budgets, so do not supply `--cycles`,
`--seconds`, `--until-frame`, `--until-pc`, `--create-initial-dump` or deferred
trace flags on the command line as well. Every `run` or `call` has its own
positive `cycles` bound. A successful batch exits 0, including calls that
stop on their expected return PC. Invalid input exits 4; a failed assertion,
missed breakpoint, exhausted call budget or unexpected CPU stop exits 9.
File I/O failures exit 3. Existing invariant/output failures can still take
precedence. Output paths cannot overwrite input files or each other.

## Plan format

The root is `{"version":1,"steps":[...]}`. The following example is an isolated
Next unit fixture: it replaces guest RAM in page 30 and at `$9000`, maps page
30 at `$E000`, then calls an injected increment routine 60 times. These
addresses are examples for a fixture, not scratch space promised by an
arbitrary program.

```json
{
  "version": 1,
  "steps": [
    {"op":"set", "registers":{"IFF1":0,"IFF2":0,"EI_PENDING":0,"HALTED":0,"SP":"$BF00"}},
    {"op":"write", "page":4, "offset":"$1000", "hex":"00"},
    {"op":"write", "page":30, "offset":0, "hex":"21 00 90 34 C9"},
    {"op":"nextreg", "index":"$57", "value":30},
    {"op":"repeat", "count":60, "steps":[
      {"op":"call", "pc":"$E000", "return_pc":"$8FFE", "cycles":1000}
    ]},
    {"op":"expect_memory", "page":4, "offset":"$1000", "hex":"3C"},
    {"op":"expect_regs", "registers":{"SP":"$BF00","HL":"$9000"}},
    {"op":"read", "name":"counter", "page":4, "offset":"$1000", "length":1},
    {"op":"read_regs", "name":"after calls"}
  ]
}
```

Numbers may be unsigned JSON integers or `$`-prefixed hex strings. Hex byte
strings allow whitespace. Register names are case-sensitive. Each operation
may have an optional `name`, which labels its observation in the report.

| Operation | Fields and meaning |
| --- | --- |
| `read_state` | Optional `stack_words` and `pc_bytes` (default 16, maximum 64). Effective mappings, bounded physical symbol annotations and unclassified stack words. |
| `read_device` | Typed palette, sprite attribute/pattern or AY selection; returns only selected device values plus relevant cursor metadata. |
| `expect_device` | Same device selectors with expected `values` or `hex`; length is inferred and mismatches include bounded context. |
| `set` | `registers`: object of register names and values. Changes only those registers. |
| `write` | `page`, `offset`, `hex`: patch physical RAM directly. Pages are **8K**, not dump `bank_N` 16K banks. |
| `nextreg` | `index`, `value`: apply a Next register write through the normal device path, including MMU/turbo side effects. Next machines only. |
| `run` | `cycles`: run from the current PC for this relative cycle budget. Optional `until_pc` stops before that PC executes; optional `until_page` requires its physical 8K page too. |
| `call` | `pc`, `return_pc`, `cycles`: push the return address on the current guest stack, enter the routine, stop before the return PC executes. |
| `repeat` | `count`, `steps`: repeat a nested list, retaining all machine state between repetitions. |
| `expect_regs` | `registers`: compare specified registers exactly; stop the batch on the first mismatch. |
| `expect_memory` | `page`, `offset`, `hex`: compare a physical RAM range exactly. |
| `read` | `page`, `offset`, `length`: include these bytes as `hex` in the compact report. |
| `read_regs` | No required extra fields; include the supported CPU registers in the report. |

Supported registers are AF/BC/DE/HL, AF'/BC'/DE'/HL', IX/IY, SP/PC, WZ, I/R,
IFF1/IFF2, IM, HALTED and EI_PENDING. Values are range-checked. Unknown and
duplicate fields, unknown registers, bad hex, invalid pages and overflowing
ranges are rejected. A memory operation must stay within one 8K page; split
larger ranges explicitly. Plans are at most 4 MiB, with at most 100,000
expanded operations and eight levels of nested repeat. The entire plan is
validated before any operation runs.
Step names are limited to 128 UTF-8 bytes. Expanded plans have a 32 MiB
storage budget and requested observations a 16 MiB budget; exceeding either
is an input error before execution.

## Execution details

`call` requires a writable, non-wrapping stack and a CPU that is not halted.
The entry and return PCs must differ. It changes SP/PC and two stack bytes;
the routine's RET normally restores SP. It neither changes interrupt policy
nor maps banks for you. Set IFF1/IFF2/EI_PENDING explicitly for an isolated
routine test, and use `nextreg` for the entry mapping. A routine's own bank
changes remain in place after return. Use a mapped resident bridge when that
is the program's ABI. The return sentinel is checked by CPU address; it must
not also occur in the routine's control flow. `run` with `until_page` is the
option for a breakpoint that must distinguish identical addresses in banks.

Host-side patches, register changes and call setup consume no guest cycles.
Call timing includes the routine and its RET, but not a synthetic CALL
instruction. Those edits are debugger operations, not guest bus writes or
instructions, and do not appear as such in traces. Physical `write` bypasses
guest write watchpoints, contention and display-write hooks. Use an ordinary
guest routine if those effects are what you are testing. Next register writes
use the device path. A batch does not reset RAM, devices, elapsed time, input
position or mappings between steps; use a fresh invocation for an independent
test branch. Snapshot resume still has the limitations documented for the
normal loader, so use fresh-source replay for visual checks that need it.

The report gives `ok`, `completed_steps`, relative guest `cycles` and an
`observations` array. Observations carry a zero-based expanded `step`, `name`,
relative `cycles`, and either `hex` or `cpu`. Failure adds `error` and
`failed_step`; earlier successful changes remain and later steps do not run.
Schema errors occur before execution and report zero completed steps.
Normal final outputs remain useful for inspecting the failure state.

The standalone regression is `tests/python/test_batch.py`; it needs no ROM or
assembler. See [the test guide](../tests/README.md) and the
[main README](../README.md).


## Symbols, typed values and build identity

For device selectors, `--inspect-out`, `--irq-diagnostics`, and the standalone
`--capabilities` query, see [compact diagnostics](../docs/diagnostics.md).
The new operations work with tracing disabled and require no full snapshot.

The C++ batch engine accepts `symbol` instead of `page` for memory operations,
and `symbol` instead of `pc` for calls. Symbolic operations require a matching
`--build-manifest FILE`, which also loads its SLD files. Symbolic calls check
that the target physical page is mapped; they do not change paging for you.
These calls currently require a Next machine. Numeric plans remain compatible.

`read_value`, `write_value` and `expect_value` take `type: "u8"`, `"u16le"` or
`"u32le"`. Writes/assertions also take `value`; reads return both `value` and
`hex`. Numeric register values, typed values, physical page and NEXTREG value
may instead be `{"symbol":"ConstantName"}`. A memory symbol must have an actual
physical page; an EQU constant with no page is not guessed into RAM. Optional
`offset` is a nonnegative byte displacement and cannot cross the symbol's 8K page.

```json
{"version":1,"steps":[
  {"op":"write_value","symbol":"Counter","type":"u16le","value":0},
  {"op":"call","symbol":"Increment","return_pc":36863,"cycles":1000},
  {"op":"expect_value","name":"counter increments","symbol":"Counter","type":"u16le","value":1}
]}
```

Execution failures include a `failure` object with step name, nested repeat
path (zero-based), symbol, physical address, CPU registers and Next MMU slots.
Memory failures include up to 32 expected/actual bytes starting at the first
mismatch. The original top-level `failed_step` remains the expanded zero-based
index. Invalid plans execute no operations. Full dumps remain separate files.

See [shared tools](../tools/README.md) for the manifest generator, snapshot
helper, layout/query command and bank checker, including their coverage limits.
