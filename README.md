# Inspeccy 1.0.0

Inspeccy runs ZX Spectrum programs from the command line and records what
happened. Use it to reproduce a bug, inspect a stopped program, compare
screenshots or audio, and run regression tests without opening an emulator
window. It also includes a Qt trace viewer and an optional SDL frontend for
playing Spectrum Next programs.

The CLI loads SNA, Z80, TAP, TRD/SCL, NEX, NextZXOS dot commands and Timex DCK
cartridges. It models the 48K, 128K, +2A/+3, Pentagon, Timex, Spectrum Next,
ZX Evolution BaseConf and ZX-Uno to varying degrees. See the
[operator guide](docs/operator-guide.md) for supported features and limitations.
ROM images are supplied separately.

Check an installed build with `inspeccy --version`. The viewer and SDL
frontend also accept `--version` and show it in their window titles.
The release number comes from [VERSION](VERSION); JSON capabilities,
status reports and dumps record it as `tool_version`. File-format and
protocol version numbers are separate and do not change with each release.

## Build

You need CMake 3.16 or newer and a C++17 compiler. From this directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DINSPECCY_BUILD_VIEWER=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

With Makefiles or Ninja, the CLI is `build/inspeccy/inspeccy`. With Visual
Studio it is `build/inspeccy/Release/inspeccy.exe`. Add that directory to your
PATH, or use the full executable path in the examples below.

Python 3.10+ enables the CLI tests; sjasmplus is needed for assembly tests.
The default CTest checks create their own inputs and do not need ROMs.
The larger [regression suite](tests/README.md) has optional prerequisites.

Optional builds:

- **Trace viewer:** enable `INSPECCY_BUILD_VIEWER` and supply Qt 6.2+ with
  Widgets and Concurrent through `CMAKE_PREFIX_PATH` or `QTDIR`. CMake reports
  when Qt is missing and the viewer is skipped.
- **Interactive emulator:** enable `INSPECCY_BUILD_EMUL` and install SDL2.
  On Windows, set `SDL2_DIR` to your SDL2 SDK's `cmake` directory. See the
  [frontend guide](inspeccy-emul/README.md) for desktop, handheld and web builds.
- **Without tracing:** set `INSPECCY_TRACE_SUPPORT=OFF`. Traces and the tests
  that require them are unavailable in that build.

`cmake --install build --config Release --prefix stage` installs the tools
and guides. On Windows it runs `windeployqt` when available to deploy the Qt
runtime. If that tool is absent, deploy the runtime and platform plugins separately.

## Try a run

This small included workload runs without firmware:

```sh
inspeccy --nex tests/data/nex/trace_workload.nex --cycles 600000 --out run
```

It writes `run.inspec.dump.json`, `run.bmp` and `run.status.json`. For an
execution trace:

```sh
inspeccy --nex tests/data/nex/trace_workload.nex --cycles 600000 --trace-out-bin run.btr --no-bmp --no-status --no-dump
inspeccy --trace-decode run.btr
```

Open `run.btr` in [inspeccy-viewer](inspeccy-viewer/README.md) to inspect it.
For a classic snapshot, provide the matching ROM:

```sh
inspeccy --sna program.sna --rom48128 roms/128.rom --seconds 2
```

A combined 32 KB 128K image contains the editor ROM followed by the 48K BASIC
ROM. `--rom48` also accepts a standalone 16 KB image. Tape and disk booting
need the appropriate system ROMs; TR-DOS also needs its disk ROM.

## Debug a program

A [JSON batch](docs/batch.md) can patch memory, run for a bounded time, call
routines and check results in one process. A build manifest pairs an image
with its sjasmplus symbols, so symbolic checks can reject a mismatched build.
[Diagnostics](docs/diagnostics.md) report the current mapping, selected device
state and interrupt events without requiring a full trace.

The [tools guide](tools/README.md) covers assembly queries, manifests, Python
adapters and capture helpers. Optional [skills for Codex and Claude Code](plugins/inspeccy-skills/README.md)
use these same command-line interfaces. They require an installed Inspeccy
executable; no MCP server is needed.

## Source layout

| Directory | Contents |
| --- | --- |
| `core/` | CPU, memory, video, sound and peripheral models |
| `shared/` | Trace formats, disassembly, symbols and common utilities |
| `inspeccy/` | Command-line frontend |
| `inspeccy-viewer/` | Qt trace inspector |
| `inspeccy-emul/` | SDL frontend and platform packaging |
| `companions/` | Image/audio comparison tools and a NextPi supervisor shim |
| `tests/` | C++ checks, Python tests, assembly cases and documented fixtures |
| `tools/` | Development and capture scripts |
| `plugins/` | Agent skill packages |
| `docs/` | User and interface guides |

Inspeccy is licensed under the [BSD 2-Clause license](LICENSE).
[Test data provenance](tests/data/README.md) distinguishes synthetic fixtures
from the author's sample programs. ROMs and external SDKs are not included.
