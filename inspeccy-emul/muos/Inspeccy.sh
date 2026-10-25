#!/bin/bash
# The inspeccy-emul ZX Spectrum Next emulator: a PortMaster-style port for
# muOS and the other aarch64 handheld firmwares.  It runs a .nex given as its
# argument, or else the first .nex in its own folder (ports/inspeccy).  With
# PortMaster present its runtime supplies the device's controller mapping,
# library path and helpers; without it the program runs on its own with the
# controller database beside it.
#
# Nothing here is allowed to fail silently: the log is opened before anything
# else is attempted, in /tmp first (always writable) and copied next to the
# game at the end, so even "the folder is not where I thought" is recorded.

LOG=/tmp/inspeccy-port.log
exec > "$LOG" 2>&1
set -x

echo "inspeccy port starting: $(date)"
echo "script: $0"
uname -a
id

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/muos/PortMaster/" ]; then
  controlfolder="/opt/muos/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi
echo "PortMaster control folder: $controlfolder"

GAMEDIR=""
if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  type get_controls >/dev/null 2>&1 && get_controls
  [ -n "$directory" ] && GAMEDIR="/$directory/ports/inspeccy"
fi

# Without PortMaster, or when its $directory does not hold the game, look
# where this script itself is: ROMS/PORTS/Inspeccy.sh -> ports/inspeccy.
if [ ! -f "$GAMEDIR/inspeccy-emul" ]; then
  HERE="$(cd "$(dirname "$0")" && pwd)"
  for TRY in "$HERE/../../ports/inspeccy" "$HERE/../../../ports/inspeccy" \
             "/mnt/mmc/ports/inspeccy" "/mnt/sdcard/ports/inspeccy" "/roms/ports/inspeccy"; do
    if [ -f "$TRY/inspeccy-emul" ]; then GAMEDIR="$(cd "$TRY" && pwd)"; break; fi
  done
fi

if [ ! -f "$GAMEDIR/inspeccy-emul" ]; then
  echo "FATAL: the game folder was not found; looked from $0"
  ls -la "$(dirname "$0")/../../" 2>&1 | head -20
  exit 1
fi
echo "game folder: $GAMEDIR"
cd "$GAMEDIR" || exit 1
ls -la

# Anything PortMaster gave us, and our own fallbacks.
ESUDO=${ESUDO:-}
$ESUDO chmod +x "$GAMEDIR/inspeccy-emul" 2>/dev/null || chmod +x "$GAMEDIR/inspeccy-emul" 2>/dev/null
[ -d "$GAMEDIR/libs" ] && export LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
echo "SDL_GAMECONTROLLERCONFIG=${SDL_GAMECONTROLLERCONFIG:-(none)}"
echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-(none)}"
ldd "$GAMEDIR/inspeccy-emul" 2>&1 | head

# The buttons as keys as well, through PortMaster's gptokeyb: the program
# reads the joystick itself, so this is only a safety net on a firmware whose
# pad SDL cannot see.
if [ -n "$GPTOKEYB" ] && [ -x "$GPTOKEYB" ]; then
  $GPTOKEYB "inspeccy-emul" -c "$GAMEDIR/inspeccy.gptk" &
  GPTOKEYB_PID=$!
fi
type pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/inspeccy-emul"

NEX="$1"
if [ -z "$NEX" ] || [ ! -f "$NEX" ]; then
  NEX=$(ls "$GAMEDIR"/*.nex "$GAMEDIR"/*.NEX 2>/dev/null | head -1)
fi
if [ -z "$NEX" ]; then
  echo "FATAL: no .nex given and none in $GAMEDIR: copy a ZX Spectrum Next program there"
  set +x
  cp "$LOG" "$GAMEDIR/log.txt" 2>/dev/null
  exit 1
fi
echo "program: $NEX"
# its files (settings, saves) live beside it
./inspeccy-emul "$NEX" --vfs "$(dirname "$NEX")" --crop 16 --fullscreen
RC=$?
echo "inspeccy-emul exit code $RC"

[ -n "$GPTOKEYB_PID" ] && kill -9 "$GPTOKEYB_PID" 2>/dev/null
type pm_finish >/dev/null 2>&1 && pm_finish

set +x
cp "$LOG" "$GAMEDIR/log.txt" 2>/dev/null
exit $RC
