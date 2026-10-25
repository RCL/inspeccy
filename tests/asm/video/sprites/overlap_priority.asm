; sprite z-priority via nr_15 bit 6. same two overlapping sprites as overlap.asm
; (slot 0 = red, slot 1 = blue, both at (128,96)), but with nr_15 bit 6 set.
;
; overlap.asm (bit 6 = 0, default): higher slot on top -> blue wins.
; here     (bit 6 = 1)            : sprite 0 on top    -> red wins.
; together they pin both directions of the priority bit.

	include "common.inc"

	silence_ula
	set_nr $09, $10	; lockstep
	set_nr $15, $41	; sprite engine on (bit0) + sprite-0-on-top (bit6)
	set_nr $4b, $e3
	set_pal_entry 2, 1, $e0, $00	; entry 1 = red
	set_pal_entry 2, 2, $03, $01	; entry 2 = blue

	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, patternred
	ld bc, $005b
	otir

	ld a, 1
	ld bc, $303b
	out (c), a
	ld hl, patternblue
	ld bc, $005b
	otir

	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attrred
	ld bc, $0457
	otir

	ld a, 1
	ld bc, $303b
	out (c), a
	ld hl, attrblue
	ld bc, $0457
	otir

	pass

patternred:
	dup 256
	db $01
	edup
patternblue:
	dup 256
	db $02
	edup
attrred:
	db 128, 96, $00, $80	; slot 0 red, pattern 0
attrblue:
	db 128, 96, $00, $81	; slot 1 blue, pattern 1
