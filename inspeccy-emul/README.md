# inspeccy-emul

A small real-time SDL2 front end over `inspeccy_core`, to play a ZX Spectrum
Next `.nex` on a Linux or Windows desktop or an OpenDingux handheld (RG350M
and kin).
It is a separate executable, disabled by default. Enable it with
`-DINSPECCY_BUILD_EMUL=ON`. It uses the same CPU and device models as the CLI.

The display texture is 640x256, preserving both pixels in the Next's 640-wide
Layer 2 mode. The default renderer samples the state at the end of a frame;
`--pixel-accurate` uses the beam renderer for Copper and other raster effects.
The frontend provides three Next AY chips, with register writes sampled at
frame boundaries, plus DAC audio.

A NEX starts with the CLI's firmware-independent setup. This is enough for
programs that stay in RAM and use the emulated NextZXOS file calls; it is not
a full NextZXOS installation. The virtual card defaults to the program's
folder when writable, otherwise `~/.inspeccy-emul/<program>/`.

## Desktop Linux

    sudo apt install libsdl2-dev cmake ninja-build
    cmake -S . -B build-emul -G Ninja -DINSPECCY_BUILD_EMUL=ON \
          -DINSPECCY_BUILD_VIEWER=OFF -DBUILD_TESTING=OFF -DINSPECCY_TRACE_SUPPORT=OFF
    cmake --build build-emul --target inspeccy-emul
    build-emul/inspeccy-emul/inspeccy-emul program.nex [--hz 50|60] [--scale 3] [--crop 16] [--fullscreen]

`--bench N` runs N frames flat out with no window and prints the rate (add
`--no-compose` for the emulation alone); it also writes the last frame as
`inspeccy-emul-bench.ppm` and the reference compositor's `...-ref.bmp` of
the same state, to compare.

## Windows

Install an SDL2 development SDK and point `SDL2_DIR` at its `cmake` folder.
Use a package matching the compiler and architecture, for example the x64
Visual C++ SDK for this build:

```powershell
cmake -S . -B build-emul -G "Visual Studio 17 2022" -A x64 -DINSPECCY_BUILD_EMUL=ON -DINSPECCY_BUILD_VIEWER=OFF -DSDL2_DIR=C:/SDK/SDL2/cmake
cmake --build build-emul --config Release --target inspeccy-emul
build-emul/inspeccy-emul/Release/inspeccy-emul.exe program.nex --scale 3
```

`SDL2.dll` is copied beside the .exe after every build and installed to
`bin/` with it. The program's files go beside the `.nex` when that folder is
writable, else under `%USERPROFILE%\.inspeccy-emul\<program>\`.

Keys: the Spectrum's letters, digits, ENTER and SPACE; Shift = CAPS SHIFT,
Ctrl = SYMBOL SHIFT, Escape = EDIT (CAPS + 1), Backspace = DELETE; the cursor
keys and right Ctrl / right Alt / right Shift are Kempston 1 directions and
fire 1 / 2 / 3. A game controller maps D-pad and left stick to the Kempston,
A / B / Y to fire 1 / 2 / 3, X to SPACE, Start to ENTER, Back to EDIT; Back
and Start together, or Guide, leave. F10 quits, F11 toggles full screen.

## RG350M (OpenDingux beta)

Toolchain: the OpenDingux gcw0 toolchain (od-2022.09.22 or later, GCC 11,
uClibc, SDL2 in the sysroot) unpacked at `/opt/gcw0-toolchain`, or point
`OPENDINGUX_TOOLCHAIN` at it. `squashfs-tools` for the package.

    cmake -S . -B build-gcw0 -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_TOOLCHAIN_FILE=inspeccy-emul/cmake/opendingux-gcw0.cmake \
          -DINSPECCY_BUILD_EMUL=ON -DINSPECCY_BUILD_VIEWER=OFF -DBUILD_TESTING=OFF \
          -DINSPECCY_TRACE_SUPPORT=OFF
    cmake --build build-gcw0 --target inspeccy-emul
    /opt/gcw0-toolchain/usr/bin/mipsel-linux-strip build-gcw0/inspeccy-emul/inspeccy-emul
    inspeccy-emul/opk/make_opk.sh build-gcw0 path/to/program.nex

That writes `build-gcw0/inspeccy-emul.opk`. Copy it to `apps/` on the
handheld's SD card and select a NEX through the launcher's file selector.
Optional NEX arguments to the packager are copied into the package. Include
only programs you are allowed to distribute.

The handheld build maps the D-pad and face buttons to Kempston controls.
START is ENTER and SELECT is SPACE; SELECT+START or POWER exits.

## muOS and other aarch64 handhelds (RG35XX Plus / H, TrimUI, ...)

`muos/` holds a PortMaster-style port of the emulator alone: `Inspeccy.sh`
for `ROMS/PORTS/` and a `ports/inspeccy/` folder with the binary. An optional
`gamecontrollerdb.txt` can supply additional SDL mappings. It runs a `.nex` given as its argument, or else the
first one in `ports/inspeccy/`. `muos/make_port.sh <binary> <out.zip>`
assembles the zip; no program is bundled.

Build the binary against an old glibc and SDL so it asks nothing a 2023+
firmware lacks: an Ubuntu 20.04 arm64 chroot (glibc 2.31, SDL 2.0.10) run
through `qemu-user-static`, which yields a binary needing only GLIBC_2.17
and `libSDL2-2.0.so.0`. `debootstrap --arch=arm64 --components=main,universe
--include=g++,cmake,ninja-build,libsdl2-dev focal <dir>
http://ports.ubuntu.com/ubuntu-ports`, then configure inside it with
`-DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"`.

The launch script finds its folder whether or not PortMaster is installed,
takes PortMaster's controller configuration when there is one, and writes
everything to `ports/inspeccy/log.txt`.

## Recalbox (Raspberry Pi 4 / CM4)

Recalbox 9.x on a Pi 4 or CM4 is 64-bit ARM with glibc and SDL2, so the
aarch64 binary built for muOS above runs there too. Recalbox launches every
game through its configgen, which runs a fixed binary per emulator, so the
emulator takes Frotz's place: `recalbox/hack-sfrotz` becomes `/usr/bin/sfrotz`
in the persistent overlay and runs inspeccy-emul on the chosen `.nex`, and a
ZX Spectrum Next system (`recalbox/zxnext-system.xml`, routed through the
frotz emulator) is added to the share's `systemlist.xml`.
`recalbox/hack-install.sh` does both, once, over SSH. Frotz stops working.

`recalbox/make_port.sh <binary> <out.zip>` assembles a zip that mirrors
`/recalbox/share`: `system/inspeccy-emul/` (the emulator, the replacement
sfrotz, its installer and `play.sh` for launching from SSH) and an empty
`roms/zxnext/` for the programs. No program is bundled.

## A browser (WebAssembly)

`web/make_web.sh <out dir> [program.nex]` builds the page with Emscripten
(tested with 3.1.61) and assembles the four files it needs. SDL2 comes from
Emscripten's own ports, so nothing else is required:

    source ~/emsdk/emsdk_env.sh
    inspeccy-emul/web/make_web.sh /tmp/web program.nex
    (cd /tmp/web && python3 -m http.server 8000)

The browser cannot block, so the same frame body runs from
requestAnimationFrame instead of its own loop; everything else is the code
the other targets use. An optional program is packed into the page at `/game`. Without one, the
page fetches `program.nex` beside it, or the filename selected by `?nex=NAME`. Its
its files (settings, results) live in the browser's IndexedDB at `/data`,
written back a second after the emulated program stops writing and again
when the page closes. A browser with storage blocked plays without keeping
anything rather than refusing to start.

`web/shell.html` is the page itself: it shows what the emulator says,
including a frames-a-second line every five seconds. `?autostart=1` skips
the Play button and `?hz=50` picks the Next's 50 Hz timing.

## When a device shows nothing

The program names its SDL version, the video drivers, the renderer, the
window size and the drawing path on start, so the log says how far it got.
If a firmware's renderer is broken, `--surface` skips it and blits into the
window surface instead, which every SDL video driver supports; the program
falls back to that by itself when no renderer or texture can be created.

## Measuring performance

Run `inspeccy-emul program.nex --bench 600` on the target device. Compare it
with `--no-compose` to separate emulation time from display composition.
Measure the program and rendering mode you intend to use; desktop results
do not predict handheld performance.

## Embedding without a bundled game

Leave `INSPECCY_EMUL_WASM_NEX` empty to build just the JS/WASM runtime.
A host page can supply `Module.canvas`, create `/game` and `/data` in
`Module.preRun`, write its NEX and saved files with `Module.FS.writeFile`,
then call `Module.callMain(['/game/game.nex', '--hz', '50', '--crop', '0'])`
after `onRuntimeInitialized`. The canvas must have `id="canvas"` for SDL2.
Use a matching cache/version key for **both** generated JS and WASM; their
import ABI changes together. Mixing cached files can produce misleading
errors such as `querySelector('-100')` during startup.

The host hooks are optional:

* `Module.paused`: pause emulation and sound, dropping held keys. Hidden
  documents also pause. Resume resets the deadline instead of catching up
  all the time the page spent hidden.
* `Module.onSaveFiles()`: called after virtual card writes settle. A custom
  host can persist files itself; without this callback the standalone shell
  keeps using IDBFS. The host should also save on `pagehide`.
* `Module.onFrame(milliseconds, presented)`: elapsed synchronous work for each
  emulated frame, including emulation, synthesis and any SDL submission,
  excluding the wait for the next frame. `presented` is false when catching
  up without presenting. Include those calls when measuring CPU cost; only
  counting presented frames hides work under load. This is not a process
  CPU-usage measurement.

The browser now gates emulated frames by the selected machine's deadline,
so 50 Hz games do not run at the monitor's 60/120/144 Hz refresh rate.
The frontend supplies three independent Next AYs (`$FF/$FE/$FD`) and mixes
ABC/ACB or per-chip mono from NextRegs 08/09. The synthesiser uses the proper
tone/noise clock divider and retriggers unchanged envelope-shape writes.
`next_three_ay` and `emul_ay_audio` are the native CTest regressions.

`--pixel-accurate` connects the existing core `BeamDrawer`, using the same
display-write hooks, beam phase and frame-boundary handling as the CLI.
The frontend copies its last completed BGR frame straight to SDL. It never
finishes an incomplete frame early or reconstructs it from the final register
state. It works in `--bench` too; `--no-compose` cannot be combined with it.
The standalone web shell exposes it as `?renderer=accurate`. The default
native/standalone mode remains the faster end-of-frame snapshot, which cannot
reproduce Copper raster splits.

The beam renderer limits sprite rasterisation and its temporary pixel buffers
to the current beam span. Previously every display-memory write could rebuild
a 320x256 sprite image, even when only a handful of pixels were needed. The
span clipping retains the same live attributes, patterns, palette and priority
rules; no sprite state is cached across writes. It applies to the CLI's beam
captures as well as this frontend, and does not require Copper to be running.

The native `beam_completed_frame` test checks split colours, row orientation,
stride and isolation from an unfinished next frame. It also compares tiny
sprite spans against whole-frame composition, including scaling, transforms,
relative sprites, clipping, 4-bit patterns and changes within a row.
`emul_copper_frame` runs
the actual SDL frontend with a generated NEX at 50 and 60 Hz, changes turbo
and keeps interrupts disabled, and verifies Copper's upper/lower colours in
the output. No ROM, assembler or display server is needed for these tests.

AY writes remain sampled at frame boundaries, without intra-frame audio timing. The optional
third AY is wired by this frontend; the CLI's capture/dump formats still
describe their existing one/two-chip setup.

