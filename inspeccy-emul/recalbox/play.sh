#!/bin/bash
# Play a ZX Spectrum Next .nex on Recalbox from an SSH shell, on the TV.
# Recalbox only frees the screen when EmulationStation steps aside, so this
# stops it, runs the game, and always brings it back -- even on error or
# Ctrl-C -- so you are never left on a black screen.
#
#   bash /recalbox/share/system/inspeccy-emul/play.sh [game.nex]
#
# With no argument it runs the first .nex in roms/zxnext.

HERE="$(cd "$(dirname "$0")" && pwd)"
ROMS=/recalbox/share/roms/zxnext
ROM="${1:-$(ls "$ROMS"/*.nex "$ROMS"/*.NEX 2>/dev/null | head -1)}"
if [ -z "$ROM" ] || [ ! -f "$ROM" ]; then
  echo "no .nex found (looked in $ROMS)"; exit 1
fi

# always restart EmulationStation on the way out
restart_es() {
  /etc/init.d/S31emulationstation start >/dev/null 2>&1 \
    || /etc/init.d/S31emulationstation restart >/dev/null 2>&1 &
}
trap restart_es EXIT

# the share is noexec: run from tmpfs
BIN=/tmp/inspeccy-emul
cp -f "$HERE/inspeccy-emul" "$BIN" && chmod +x "$BIN"
[ -f "$HERE/gamecontrollerdb.txt" ] && cp -f "$HERE/gamecontrollerdb.txt" /tmp/

# free the screen
/etc/init.d/S31emulationstation stop >/dev/null 2>&1 || killall emulationstation 2>/dev/null
sleep 2

cd /tmp
"$BIN" "$ROM" --vfs "$ROMS" --crop 16 --fullscreen
echo "game exited $?"
