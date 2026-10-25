inspeccy-emul in a browser
==========================

Served over http (a browser will not load WebAssembly from a file:// page):

  inspeccy-emul.html   the page
  inspeccy-emul.js     the loader
  inspeccy-emul.wasm   the emulator
  program.nex          the ZX Spectrum Next program, fetched at start-up

The player is not built for one program: it fetches the .nex beside it, so
a new version is a file copy, not a rebuild.  make_web.sh can also pack a
program into the page (as inspeccy-emul.data) for a self-contained bundle.

Try it locally with any static server, for example:

  python3 -m http.server 8000      # then open http://localhost:8000/inspeccy-emul.html

Embedding it in a page of your own: an <iframe> pointed at
inspeccy-emul.html works as it is. URL parameters (combine with &):

  ?nex=NAME      which program to run: a file beside the page, or one of
                 several packed in.  Default program.nex
  ?autostart=1   begin without the Play button (sound still needs a click)
  ?hz=50         run the Next at 50 Hz; ?hz=60 for 60; ?hz=auto measures the
                 display once before boot and picks 60 only for a steady
                 60 Hz one
  ?crop=N        hide N rows at the top and bottom, for a program that
                 leaves them blank at 60 Hz
  ?renderer=accurate   use the pixel-accurate beam renderer (more CPU work)

The program's settings and results are kept in the browser's IndexedDB, so
they survive a reload; "Clear saved settings" on the page removes them.  A
browser with storage blocked simply plays without keeping anything.

The page prints what the emulator says, including how many frames a second
it is managing, every five seconds.  If it runs slowly, that line is the
number to report.
