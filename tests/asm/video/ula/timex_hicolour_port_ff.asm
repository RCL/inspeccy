; zx spectrum next, timex 8x1 hi-colour selected the ordinary way: out ($ff),2.
;
; nextreg $69 bits 5:0 alias port $ff's timex display-mode bits.
; selecting mode 2 must read per-line attributes at $6000.
;
; what this catches: the $ff decode going away for the next, and the renderer
; forgetting the mode and falling back to the 8x8 attribute plane. either way
; the reference image turns into one flat colour.

	include "common.inc"
	include "video/ula/_timex_hicolour_screen.inc"

	ld a, 2	; timex mode 2 = 8x1 hi-colour
	out ($ff), a

	pass
