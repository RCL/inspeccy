; transparency with a green ULA border and the reset black fallback ($4a=0).
; transparent display pixels must use black, independent of the border colour.
; narrow modes retain the green outer border; wide Layer 2 covers the canvas.

	include "common.inc"

	silence_ula

	ld a, 4	; border green
	out ($fe), a

	set_pal_entry 0, 1, $e3, $00
	set_nr $6a, $00
	set_nr $15, $80

	ld hl, $4000
	ld de, $4001
	ld bc, $17ff
	ld (hl), 1
	ldir

	ld hl, $6000
	ld de, $6001
	ld bc, $17ff
	ld (hl), 1
	ldir

	pass
