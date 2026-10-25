; nr_15 bit 1 is clear: clip sprites to x=32..287, y=32..223.
; one sprite is inside, one straddles the top boundary, and one is wholly
; outside the right edge. only the first and half of the second render.

	include "common.inc"

	silence_ula
	set_nr $09, $10	; sprite-id lockstep
	set_nr $15, $01	; bit 0 = engine on, bit 1 = off (default)
	set_nr $4b, $e3
	set_pal_entry 2, 1, $e0, $00	; red

; pattern slot 0 - solid 16x16 red.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, pattern
	ld bc, $005b
	otir

; three attributes back-to-back.
	ld a, 0
	ld bc, $303b
	out (c), a
	ld hl, attr
	ld bc, $0c57	; b=$0c = 12 bytes = 3 sprites
	otir

	pass

pattern:
	dup 256
	db $01
	edup

attr:
; sprite 0 - centre of ula area. visible.
	db 128	; x = 128
	db 96	; y = 96
	db $00
	db $80

; sprite 1 - straddles the top-border / ula boundary at y=32.
; sprite bbox = y 24..39: y 24..31 in border (clipped), y 32..39 visible.
	db 160	; x
	db 24	; y = 24
	db $00
	db $80

; sprite 2 - fully in the right border (x=290..305, all > 287).
	db 34	; x lo = 34, x8 = 1 -> x = 256 + 34 = 290
	db 96	; y
	db $01	; x8 = 1 (bit 0 of attr2)
	db $80
