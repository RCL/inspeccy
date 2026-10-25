#!/bin/bash
# Assembles the Recalbox package of the emulator alone:
#   make_port.sh <aarch64 inspeccy-emul binary> <out.zip>
# The zip mirrors /recalbox/share: system/inspeccy-emul/ (the emulator, the
# replacement sfrotz and its installer) and an empty roms/zxnext/ for the
# programs.  No program is bundled.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${1:?aarch64 inspeccy-emul}; OUT=${2:?output zip}
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
chmod 755 "$STAGE"
D="$STAGE/share/system/inspeccy-emul"
mkdir -p "$D" "$STAGE/share/roms/zxnext"
cp "$BIN" "$D/inspeccy-emul"
cp "$HERE/hack-sfrotz" "$HERE/hack-install.sh" "$HERE/zxnext-system.xml" "$HERE/play.sh" "$HERE/README.txt" "$D/"
[ -f "$HERE/../muos/gamecontrollerdb.txt" ] && cp "$HERE/../muos/gamecontrollerdb.txt" "$D/"
chmod 755 "$D/inspeccy-emul" "$D/hack-sfrotz" "$D/hack-install.sh" "$D/play.sh"
{
  echo "packed:    $(date -u '+%Y-%m-%d %H:%M:%SZ')"
  echo "emulator:  $(basename "$BIN")  $(stat -c %s "$BIN") bytes  sha1 $(sha1sum "$BIN" | cut -d' ' -f1)"
} > "$D/BUILD.txt"
cat "$D/BUILD.txt"
rm -f "$OUT"
(cd "$STAGE" && zip -q -r "$OUT" share)
unzip -l "$OUT"
