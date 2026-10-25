#!/bin/sh
# inspeccy-emul from inside the .opk: the argument is the .nex the launcher
# chose (%f). Everything the
# program says goes to inspeccy-emul.log on the external card (or in the home
# directory without one), since the launcher shows nothing of it.
cd "$(dirname "$0")" || exit 1
if [ -w /media/sdcard ]; then LOG=/media/sdcard/inspeccy-emul.log; else LOG="${HOME:-/media/data/local/home}/inspeccy-emul.log"; fi
{
  echo "inspeccy-emul run.sh started: $(date) in $(pwd), args: $*"
  echo "--- loader check"
  LD_TRACE_LOADED_OBJECTS=1 ./inspeccy-emul 2>&1
  echo "--- program"
} > "$LOG" 2>&1
if [ -n "$1" ]; then
  ./inspeccy-emul "$@" >> "$LOG" 2>&1
else
  echo "Choose a .nex program in the launcher." >> "$LOG"
  exit 2
fi
echo "--- exit code $?" >> "$LOG"
