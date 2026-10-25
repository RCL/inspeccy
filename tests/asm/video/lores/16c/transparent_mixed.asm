; lores 16c per-pixel transparency: byte $12 -> left nibble = 1 (red,
; opaque), right nibble = 2 (transparent). the whole framebuffer is $12,
; so every adjacent pair of lores pixels is (red, transparent). after 2x
; scaling each pair is 4 composite pixels wide: (red, red, black, black).
;
; what this catches: a 16c decoder that doesn't honour per-pixel (= per-
; nibble) transparency, e.g. one that only checks the left nibble.

	include "common.inc"

	silence_ula
	set_pal_entry 0, 1, $e0, $00	; red (opaque)
	set_pal_entry 0, 2, $e3, $00	; transparent
	set_nr $6a, $20
	set_nr $15, $80

	ld hl, $4000
	ld de, $4001
	ld bc, 6143
	ld (hl), $12	; left=opaque, right=transparent
	ldir

	pass
