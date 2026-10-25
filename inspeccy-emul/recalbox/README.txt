inspeccy-emul for Recalbox (Raspberry Pi 4 / CM4, 64-bit)
=========================================================

A ZX Spectrum Next emulator that takes the place of Recalbox's Frotz, so
a ZX Spectrum Next system appears in the menu and launches with the
controller. No program comes with it: bring your own .nex. Frotz (the
interactive-fiction emulator) stops working.

INSTALL
  1. Copy the share folder's contents onto Recalbox's share, keeping the
     names (over the network: \\RECALBOX\share):
       share/system/inspeccy-emul/  ->  /recalbox/share/system/inspeccy-emul/
       share/roms/zxnext/           ->  /recalbox/share/roms/zxnext/
  2. Once, over SSH (user root, password recalboxroot by default):
       bash /recalbox/share/system/inspeccy-emul/hack-install.sh
     It puts the replacement sfrotz in the persistent overlay (it should
     report an ext4 partition, not tmpfs) and adds the ZX Spectrum Next
     system to /recalbox/share/system/.emulationstation/systemlist.xml.
  3. Put .nex files in roms/zxnext and reboot.

  To update the emulator later, replace system/inspeccy-emul/inspeccy-emul;
  the installer need not run again.

CONTROLS
  D-pad / left stick   Kempston joystick
  A / B / Y            fire 1 / 2 / 3
  X                    SPACE
  START                ENTER
  SELECT               EDIT (CAPS SHIFT + 1)
  SELECT + START       leave the emulator

IF IT DOES NOT START
  Read roms/zxnext/log.txt: it records how it was launched and any error.
  From SSH, play.sh runs a .nex on the TV directly:
    bash /recalbox/share/system/inspeccy-emul/play.sh [game.nex]
