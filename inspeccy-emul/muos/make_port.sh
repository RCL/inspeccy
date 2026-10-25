#!/bin/bash
# Assembles the muOS / PortMaster-style port of the emulator alone, from an
# aarch64 build of inspeccy-emul: make_port.sh <inspeccy-emul binary> <out.zip>
# The zip holds ROMS/PORTS/Inspeccy.sh and ports/inspeccy/; no program is
# bundled: the user copies .nex files into ports/inspeccy/.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${1:?aarch64 inspeccy-emul}; OUT=${2:?output zip}
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/ROMS/PORTS" "$STAGE/ports/inspeccy"
cp "$HERE/Inspeccy.sh" "$STAGE/ROMS/PORTS/"
cp "$BIN" "$STAGE/ports/inspeccy/inspeccy-emul"
cp "$HERE/inspeccy.gptk" "$HERE/README.txt" "$STAGE/ports/inspeccy/"
[ -f "$HERE/gamecontrollerdb.txt" ] && cp "$HERE/gamecontrollerdb.txt" "$STAGE/ports/inspeccy/"
chmod 755 "$STAGE/ROMS/PORTS/Inspeccy.sh" "$STAGE/ports/inspeccy/inspeccy-emul"
{
  echo "packed:    $(date -u '+%Y-%m-%d %H:%M:%SZ')"
  echo "emulator:  $(basename "$BIN")  $(stat -c %s "$BIN") bytes  sha1 $(sha1sum "$BIN" | cut -d' ' -f1)"
} > "$STAGE/ports/inspeccy/BUILD.txt"
cat "$STAGE/ports/inspeccy/BUILD.txt"
rm -f "$OUT"
(cd "$STAGE" && zip -q -r "$OUT" ROMS ports)
unzip -l "$OUT"
