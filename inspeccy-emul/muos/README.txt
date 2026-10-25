inspeccy-emul for muOS (RG35XX Plus / H and other aarch64 handhelds)
====================================================================

A ZX Spectrum Next emulator, packaged the way PortMaster ports are. No
program comes with it: bring your own .nex.

INSTALL
  Copy the two folders onto the SD card, keeping the names:
    ROMS/PORTS/Inspeccy.sh           (the entry that appears under Ports)
    ports/inspeccy/                  (the emulator)
  On muOS these live on the first card (SD1). Then put a .nex in
  ports/inspeccy/, refresh the Ports list (or reboot) and start Inspeccy.
  It runs the first .nex in that folder. A program's own files (settings,
  saves) are kept beside it.

CONTROLS
  D-pad / left stick   Kempston joystick
  A / B / Y            fire 1 / 2 / 3
  X                    SPACE
  START                ENTER
  SELECT               EDIT (CAPS SHIFT + 1)
  SELECT + START       leave the emulator

IF IT DOES NOT START
  Read ports/inspeccy/log.txt: it records what the launcher found, the
  renderer and window, and any error.
