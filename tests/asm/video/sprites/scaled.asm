; extended sprite scaled 2x on both axes: a 32x32 red block at (96,80).
; attribute 4: x scale in bits 4:3, y scale in bits 2:1, so $0a = 2x2.
; https://wiki.specnext.dev/Sprite_Attribute_4_Register

	include "common.inc"

	silence_ula
	set_nr $09, $10
	set_nr $15, $01
	set_nr $4b, $e3
	set_pal_entry 2, 1, $e0, $00

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
	ld bc, $0557	; 5 bytes for 5-byte attr
	otir

	pass

pattern:
	dup 256
	db $01
	edup

attr:
	db 96	; attr0 = x low
	db 80	; attr1 = y
	db $00	; attr2 = palette 0, no transforms, x8 = 0
	db $c0	; attr3 = v=1, e=1, pattern 0
	db $0a	; attr4 = x scale 2, y scale 2
