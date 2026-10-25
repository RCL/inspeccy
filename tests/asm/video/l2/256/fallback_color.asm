; transparency with a green ULA border and the reset black fallback ($4a=0).
; transparent display pixels must use black, independent of the border colour.
; narrow modes retain the green outer border; wide Layer 2 covers the canvas.

	include "common.inc"

l2_bank_16k equ 9
page_base equ l2_bank_16k * 2

	silence_ula

; border colour: ula port $fe bits 0..2. green = 4.
; important: this also writes the ear / mic bits (3, 4) and bit 3 of
; the keyboard mask - we don't care about those for a video test.
	ld a, 4
	out ($fe), a

	set_nr $70, $00
	set_nr $12, l2_bank_16k
	set_nr $69, $80
	set_pal_entry 1, 1, $e3, $00	; index 1 -> transparent

	rept 6, page
	set_nr $52, page_base + page
	ld hl, $4000
	ld de, $4001
	ld bc, 8191
	ld (hl), 1
	ldir
	endr

	set_nr $52, 4

	pass
