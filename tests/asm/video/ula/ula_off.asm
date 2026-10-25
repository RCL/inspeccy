; nr_68 bit 7 = ula output off. the ula bitmap is filled with solid red (ink
; every pixel), which would normally cover the 256x192 area; setting nr_68 bit 7
; makes the whole ula layer transparent, so the paper area falls through to the
; black fallback. the outer border remains; no red is visible.

	include "common.inc"

	silence_ula
	set_nr $43, $00
	set_pal_entry 0, 2, $e0, $00	; ink 2 = red

	set_nr $52, 10	; map bank 5 to $4000
	ld hl, $4000	; pixels all $ff = every pixel ink
	ld de, $4001
	ld bc, 6143
	ld (hl), $ff
	ldir
	ld hl, $5800	; attrs $02 = ink 2, paper 0
	ld de, $5801
	ld bc, 767
	ld (hl), $02
	ldir
	set_nr $52, 4

	set_nr $68, $80	; ula output off

	pass
