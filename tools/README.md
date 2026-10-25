# Shared experiment and assembly tools

These interfaces are ordinary CLI commands and Python modules. They have no
model, plugin, ROM or server dependency. The Python here is only
the optional snapshot/batch client glue, and needs 3.10+; everything that
analyses or runs a program is in the C++ CLI, including the `--asm-*` and
`--write-manifest` commands described below.

## Where the savings come from

Use `--batch` for memory patches, calls and assertions. Return only selected
observations when possible. An interactive experiment can use a short batch,
save its resulting state, and choose the next batch after examining the result.
It need not know the whole experiment in advance. Independent branches should
start from independent states and virtual filesystems.

When Python must edit a snapshot, `inspeccy_tools.Snapshot` retains the original
text for unchanged RAM banks. It compares bank bytes on save, so direct bytearray
edits and whole-bank replacements are detected too. Changed banks use Python's
native hex formatter. Zero-elided banks are decoded as full zero-filled banks.
File references are resolved against the input snapshot before relocation.
The complete JSON is still written; this does not eliminate restore or dump I/O.

Add this directory to `PYTHONPATH` to use the optional adapters:

```python
from inspeccy_tools import Snapshot, run_batch

state = Snapshot('ready.json')
state.validate_build('program.build.json')
state.banks[2][0x1000] = 42
state.save('experiment/seed.json')
report, final_dump = run_batch('inspeccy', 'experiment/run', [
    {'op': 'read_value', 'symbol': 'Counter', 'type': 'u16le', 'name': 'counter'}
], resume='experiment/seed.json', dump=True,
   extra=['--build-manifest', 'program.build.json'])
```

`BatchError` includes the compact report and its path. The adapter defaults to
no final dump; request `dump=True` to continue from the resulting state.

## Build identity

After successful assembly/package creation, bind the final NEX to its SLD files:

```text
inspeccy --write-manifest output/program.build.json --image output/program.nex --sld output/program.sld
inspeccy --nex output/program.nex --build-manifest output/program.build.json --batch experiment.json --no-bmp
```

Manifest version 1 has `image: {path, sha1}` and a `symbols` array with the same
fields. Paths are relative to the manifest. SHA-1 here detects accidental build
mismatches; it is not an authenticity signature. The producer must generate the
manifest from one successful build, not pair arbitrary pre-existing files.

Inspeccy retains the original NEX hash across resume, even when writing a later
dump after the executable changed. An existing NEX with a different hash is
rejected. A missing original file does not prevent RAM-only resume, unless an
open streaming handle requires that file. Manifest-backed symbolic operations
also require the symbol files to match the manifest. Numeric plans remain
compatible with old dumps. Legacy dumps with no identity cannot be silently
certified by hashing today's executable.

`--allow-build-mismatch "reason"` explicitly permits intentional mismatched or
legacy state and records the reason in the next dump. It retains the original
hash (or lack of one), and still checks that SLD contents match their manifest.
Ordinary RAM edits do not change the identity of the originating build.
Version 1 program identity is for NEX; SNA/dot identity is not yet implemented.

## Layout and bounded source queries

```text
inspeccy --asm-layout --sld output/program.sld --source-root code --asm-config assembly.json
inspeccy --asm-query Routine --sld output/program.sld --source-root code --limit 40
```

Both accept `--json`. The source root is the assembler working directory, since
SLD paths are often relative to it. The index expands 16K SLD banks to physical
8K pages, preserves aliases, and resolves simple macro arguments using SLD
definition locations. Queries show bounded instruction bodies and direct
callers/callees, with omitted counts. Indirect or unresolved targets stay
explicitly unresolved. Missing source is not fabricated from the symbol map.

A layout configuration describes real reservations rather than inferring them
from the last label. Add end labels at assembly boundaries; they emit no bytes.

```json
{"version":1,"regions":[
  {"name":"Resident", "page":5, "start":40960,
   "end":"CodeEnd", "limit":"STACK_TOP-256"}
]}
```

Values may be integers or expressions using symbols, `$hex`, addition,
subtraction and multiplication. Used bytes include internal alignment padding;
free means space up to the declared limit. Stack reservations are not measured
stack high-water marks. Detailed padding classification is deferred. Keep the
assembler's assertions as well. This report rejects inverted or overfull regions.

## Bank contracts for existing assembly

```text
inspeccy --asm-banks --sld output/program.sld --source-root code --asm-config assembly.json --json
```

The same configuration may contain:

```json
{"version":1,"fixed":{"4":4,"5":5},"routines":{
  "Gate":{"end":"GateEnd","requires":{"7":31},"returns":{"7":31},"context":"frame"},
  "Target":{"external":true,"requires":{"7":30},"returns":{"7":30},"clobbers":[6],"context":"frame"}
}}
```

Declare exclusive routine ends. `requires` supplies entry page mappings;
instruction placement also requires the routine's own page. `returns` describes
postconditions, with `"entry"` meaning the mapping at this call's entry.
`clobbers` lists slots whose mapping becomes unknown. Slots absent from both
`clobbers` and `returns` are promised preserved by that contract. External
contracts are trusted assumptions, explicitly listed in the result. Without
`--json` the command prints a readable summary; either way it exits 1 when it
reports errors, and 2 on bad input.

The checker follows bounded control flow within declared ranges, immediate
NEXTREG mapping changes, direct calls/tail calls, conditional returns, and
direct symbolic memory accesses, including alternate-window expressions such
as `(Table-$2000)`. A wrong known mapping is an error. An unknown mapping,
uncontracted call, indirect data access or port-driven switch is unverified.
Execution-context declarations reject incompatible annotated callers/callees.
Indirect data users must declare their required data mappings; general pointer
provenance and saved dynamic mapping analysis are deferred. No DI/EI is inserted.

The report always states its coverage. A clean result for four gates does not
certify the rest of an application, nor independently prove external promises.
Write contracts alongside the program they describe and update them when its
mapping conventions change.

## Validation and deferred work

`tests/python/test_tooling.py` creates its own tiny NEX, SLD, source and snapshots. It
checks identity, relocation, direct RAM edits, aliases, typed batches, mapping
failures, macro-independent bank errors, layout and 16K page conversion, driving
the analysis through the CLI exactly as a caller would. `tests/cpp/test_asm_index.cpp`
covers the index, the layout table and the bank lint directly, including what the
lint refuses to conclude. `tests/python/test_batch.py` continues to cover the original
numeric interface.

Visual checkpoint caching, persistent processes/MCP, compressed dumps,
automatic bridge generation and broad pointer analysis remain deferred. Fresh
execution remains the basis for published captures. Host-side patches are not
CPU instructions and must not be assumed reproducible by frame replay alone.

The [diagnostics guide](../docs/diagnostics.md) covers stopped-state reports,
device assertions and interrupt attribution. `inspeccy --capabilities`
reports which interfaces an executable supports.

## Measurement companions

`companions/ssim-compare` and `companions/wav-compare` are two small C++
programs built alongside the emulator (and buildable on their own; neither
links `inspeccy_core` or `inspeccy_shared`).

```text
ssim-compare frames/ref frames/test --search --windows 8 --worst 5 --min-ssim 0.95
wav-compare  audio/ref.wav audio/test.wav --skip-s 1 --min-spec 0.8
```

`ssim-compare` scores greyscale SSIM over two numbered BMP sequences, reading
the 4-, 8- and 24-bit BMPs the emulator writes. `wav-compare` downmixes and
resamples to 11025 Hz, then reports spectral, envelope and raw-waveform
correlation plus a drift estimate. Both exit 0 on pass, 1 below a threshold
and 2 on invalid input; both are diagnostics, not proof of visual, musical or
hardware accuracy.

They are compiled `-O3 -ftree-vectorize -fopenmp-simd` with explicit
`#pragma omp simd reduction` on the hot accumulations, and deliberately
**not** with `-ffast-math`: their exit codes are threshold comparisons, so a
global relaxation of floating-point semantics could flip a borderline result
and make it differ between machines. `-DINSPECCY_COMPANION_NATIVE_ARCH=ON`
adds `-march=native` for a development box; it is off by default so a
checked-in binary stays portable.

## Skills for Codex and Claude Code

The [Inspeccy Skills package](../plugins/inspeccy-skills/README.md) contains three shared
skills for debugging, assembly analysis and Next graphics diagnosis. Run
`python tools/install_agent_skills.py --client both` to install personal skill copies,
or use the package's separate client manifests. The installer refuses unknown
overwrites and local edits; subsequent owned updates use `--update`.

Skills reduce repeated tool discovery and fixture/mapping mistakes. Their short
entry points load specialised recipes only when needed. Execution remains in
the same CLI for either client. No quantitative token saving or
identical model behaviour is claimed.
