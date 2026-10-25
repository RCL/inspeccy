# inspeccy-viewer

A Qt 6 inspector for binary traces produced by the inspeccy CLI.

## Workflow

1. Record a run with --trace-out-bin run.btr, or use the viewer's Trace dialog to launch the CLI with the selected source, ROMs, and budget.
2. Open the BTR file. A matching .fidx sidecar accelerates frame lookup when present. The viewer maps the trace file and builds analysis data; startup time and memory use still grow with the trace.
3. Optionally load sjasmplus SLD symbol files to label addresses.

The overview selects a frame, the icicle timeline shows inferred call depth, and the instruction list shows decoded operations and recorded register changes. Register and memory views follow the selected position.

### Longest frame ISRs, using the UI

1. **File > Trace** (`Ctrl+T`): choose the binary and its matching **SLD symbols** file. A sibling `.sld` is suggested automatically; ensure both files came from the same build. **Run for** includes startup. **Start recording after** skips recording that many initial seconds while still emulating them; it does not add to the run budget. These settings are retained in **Trace History**.
2. After **Trace and Open**, choose **View > Frame ISR executions** (`Ctrl+I`). This table covers the entire capture, irrespective of the current timeline zoom. It starts sorted by inclusive cycles, longest first. Click a heading to change the sort. Rank remains the duration rank; equal durations retain chronological order.
3. Select a row or use **Up/Down**. The cursor moves to that ISR and the timeline fits its complete cycle span, with padding and space for its call levels. **F** fits the selected ISR again; **Ctrl+0** fits the whole trace. Fitting is retained when resizing the window. In the timeline, **W/S** zoom and **A/D** pan.
4. **Ctrl+M** toggles the memory displays to give the results more width. **File > Save screenshot** (`Ctrl+Shift+S`) saves the current viewer as PNG. To capture the five longest executions, save the first row, press Down and save again, through rank 5.

The table shows the frame at ISR acceptance, start cycle, inclusive cycle duration, and the SLD entry symbol resolved against its recorded physical page. The selected row also shows the exclusive end cycle and page/address. Nested calls are included in the duration. Only accepted ULA/frame-source ISR invocations with an observed stack return are ranked; partial executions at the end of a trace and interrupts without source metadata are counted separately. Other interrupt sources are not frame ISRs. This is a cycle ranking, not an elapsed-time ranking across CPU clock changes. Call boundaries retain the reconstruction limitations below.

The trace contains the information emitted by the recorder, rather than a complete emulator checkpoint at every instruction. Memory reconstruction depends on available snapshots and recorded writes. Call depth is inferred from control flow and can be misleading for stack manipulation, unusual returns, or entry into code midway through a call chain. Treat these views as debugging aids.

## ROMs and executable

The Trace dialog saves selected paths in the application's settings. Its ROM search checks `INSPECCY_ROM_DIR`, then the older `INSPECCY_ROMS` variable, then `roms/` directories relative to the executable and working directory. `INSPECCY_ROM_DIR` is also used by the Python tests. No ROMs are included.

The viewer launches the separately built inspeccy executable. A trace-disabled CLI cannot produce its input files. RAM-only NEX programs can run without ROMs; programs calling ROM routines need suitable images.

The CLI also accepts [JSON batch experiments](../docs/batch.md), which
run a sequence of patches, calls and checks in one process. This reduces
repeated dump/resume overhead and is available to any script or model. The
viewer has no batch editor; launch the CLI with `--batch` and open any trace
it produces here. Host-side batch patches and call setup are not guest
instructions and are not represented as such in the trace.

## Build

The top-level CMake build enables the viewer when Qt 6.2 or newer, Widgets, and Concurrent are found. Set CMAKE_PREFIX_PATH to the Qt SDK as needed. -DINSPECCY_BUILD_VIEWER=OFF builds only the headless components.

On Windows, CMake runs the SDK's `windeployqt` at install time when it finds it. Otherwise, deploy the matching Qt DLLs and platform plugin before launching the installed viewer. The viewer is a trace inspector; it does not provide interactive live emulation.

## Offscreen usability capture

Qt's offscreen platform renders the actual widgets without opening desktop
windows or moving the mouse. The normal viewer supports
`inspeccy-viewer -platform offscreen run.btr --screenshot viewer.png` for a
single capture. On Windows it registers installed Segoe UI and Consolas fonts
because the offscreen plugin does not discover the native font database.

For the complete ranked-ISR workflow, build the opt-in
`inspeccy_viewer_usability` target with `BUILD_TESTING=ON`. Run it with a JSON
file of Trace dialog settings and an output directory:

```text
inspeccy_viewer_usability capture-params.json screenshots
inspeccy_viewer_usability capture-params.json screenshots --reuse
```

Example settings (use absolute paths and a matching binary/SLD pair):

```json
{
  "binary": "/path/to/captures/program.nex",
  "sld": "/path/to/captures/program.sld",
  "machine": "next50",
  "ramMb": 2,
  "runUnit": 0,
  "runVal": 80,
  "traceFromSeconds": 15,
  "output": "/path/to/captures/program.btr"
}
```

The test always selects the offscreen platform, uses isolated settings, fills
the real Trace dialog, presses **Trace and Open**, opens **Frame ISR executions**,
hides memory displays, and presses **Down** four times. It saves the dialog,
five full viewer screenshots, the capture log and the table's displayed results
as JSON. `--reuse` reopens an existing trace instead of recording again. Ranking
and fitting come from the UI; the test does not compute alternate rankings or
set numeric viewport coordinates. The equivalent human steps are above.

For a Qt SDK without deployed offscreen plugins, set `QT_PLUGIN_PATH` to its
`plugins` directory. On Windows set `QT_FORCE_STDERR_LOGGING=1` to capture test
progress in the console rather than the debugger.
