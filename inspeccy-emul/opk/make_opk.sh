#!/bin/bash
# Packs the cross-built inspeccy-emul into an OpenDingux .opk (a squashfs
# image with a desktop entry).  Copy the .opk to the handheld's SD card
# (apps/ on the external card) and it appears in the launcher.
#
#   make_opk.sh <build dir> [<program.nex> ...]
#
# Any .nex given is packed in too and offered by the launcher's file
# selector alongside those on the card; the launcher's %f passes the chosen
# file on the command line.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${1:?build directory}
shift
STAGE=$(mktemp -d)
chmod 755 "$STAGE"   # mktemp gives 0700, which would make the whole .opk root-only
trap 'rm -rf "$STAGE"' EXIT
cp "$BUILD/inspeccy-emul/inspeccy-emul" "$STAGE/"
cp "$HERE/default.gcw0.desktop" "$HERE/run.sh" "$HERE/inspeccy-emul.png" "$STAGE/"
chmod +x "$STAGE/run.sh" "$STAGE/inspeccy-emul"
for f in "$@"; do cp "$f" "$STAGE/"; done
OUT="$BUILD/inspeccy-emul.opk"
rm -f "$OUT"
mksquashfs "$STAGE" "$OUT" -all-root -noappend -no-xattrs -quiet
ls -la "$OUT"
