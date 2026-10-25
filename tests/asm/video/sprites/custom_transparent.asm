; sprite with custom nr_4b transparent index. nr_4b := $07 means pattern
; bytes equal to $07 are skipped. pattern: top half = $01 (red), bottom
; half = $07 (transparent). expected: 16x8 red block at top of sprite area.
;
; catches a renderer that hardcodes $e3 as the sprite-transparent sentinel
; instead of reading nr_4b.

	include "common.inc"

	silence_ula
	set_nr $09, $10
	set_nr $15, $01
	set_nr $4b, $07	; custom transparent index
	set_pal_entry 2, 1, $e0, $00	; red

	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, pattern
	ld bc, $005b
	otir

	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attr
	ld bc, $0457
	otir

	pass

pattern:
	dup 128
	db $01	; opaque red
	edup
	dup 128
	db $07	; transparent per nr_4b
	edup

attr:
	db 128
	db 96
	db $00
	db $80
