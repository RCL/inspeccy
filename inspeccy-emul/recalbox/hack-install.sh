#!/bin/bash
# One-shot installer for the ZX Spectrum Next system on Recalbox.  Run with:
#   bash /recalbox/share/system/inspeccy-emul/hack-install.sh
# Recalbox launches every game through its configgen, which runs a fixed
# binary per emulator.  This replaces Frotz's (/usr/bin/sfrotz) with
# hack-sfrotz, which runs inspeccy-emul instead, and adds a ZX Spectrum Next
# system routed through the frotz emulator.  Frotz itself stops working.
#
# The overlay's writable upper lives on the ext4 partition mounted at
# /overlay; a file dropped there is part of / after a reboot.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "overlay layout:"; ls -la /overlay 2>/dev/null | head
U=""
for CAND in /overlay/upper /overlay/overlay_root/upper /overlay_root/upper; do
  if mkdir -p "$CAND/usr/bin" 2>/dev/null; then U="$CAND"; break; fi
done
if [ -z "$U" ]; then echo "FATAL: no writable overlay upper found"; exit 1; fi
echo "writable upper: $U"
cp -f "$HERE/hack-sfrotz" "$U/usr/bin/sfrotz"
chmod +x "$U/usr/bin/sfrotz"
echo "installed: $(ls -l "$U/usr/bin/sfrotz")"
echo "backing fs (should be ext4, not tmpfs):"; df -T "$U" 2>/dev/null | tail -1 || mount | grep -E "on /overlay "

# The system list: the share's own copy wins over the stock one, so start
# from it if there is one, otherwise from the stock list, and add the ZX
# Spectrum Next system once.
LIST=/recalbox/share/system/.emulationstation/systemlist.xml
STOCK=/recalbox/share_init/system/.emulationstation/systemlist.xml
mkdir -p "$(dirname "$LIST")" /recalbox/share/roms/zxnext
if [ -f "$LIST" ] && grep -q 'name="zxnext"' "$LIST"; then
  echo "system list: ZX Spectrum Next already there"
else
  SRC="$LIST"; [ -f "$SRC" ] || SRC="$STOCK"
  if [ ! -f "$SRC" ]; then echo "FATAL: no systemlist.xml at $LIST or $STOCK"; exit 1; fi
  [ -f "$LIST" ] && cp -f "$LIST" "$LIST.before-zxnext"
  awk -v f="$HERE/zxnext-system.xml" '/<\/systemList>/ { while ((getline l < f) > 0) print l } { print }' "$SRC" > /tmp/systemlist.xml
  grep -q 'name="zxnext"' /tmp/systemlist.xml || { echo "FATAL: could not add the system"; exit 1; }
  cp -f /tmp/systemlist.xml "$LIST"
  echo "system list: ZX Spectrum Next added to $LIST (from $SRC)"
fi
echo "DONE. Put .nex files in /recalbox/share/roms/zxnext, reboot, then open ZX SPECTRUM NEXT."
