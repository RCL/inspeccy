; the ula palette keeps inks at 0..15 and papers at 16..31, and the border is
; a paper: entry 16 + the $fe colour. a classic screen of attribute $10 (ink
; 0, paper 2) with border 7, then the palette is reprogrammed: paper entry 18
; blue, border entry 23 green, and the ink entries 2 and 7 magenta. only the
; paper/border layout shows blue inside and green around; a renderer that
; takes them from the ink entries shows magenta for both.
;
; seen on a board: a lores logo palette whose entry 16 was violet gave a
; violet border, while entries 0..7 said nothing about it.

	include "common.inc"

	set_nr $52, 10	; map bank 5 (ula screen) to $4000
	ld hl, $4000	; no ink pixels
	ld de, $4001
	ld bc, 6143
	ld (hl), $00
	ldir
	ld hl, $5800	; attributes = $10: ink 0, paper 2
	ld de, $5801
	ld bc, 767
	ld (hl), $10
	ldir
	set_nr $52, 4

	ld a, 7	; border 7
	out ($fe), a

	set_pal_entry 0, 2, $e3, $00	; ink 2: magenta, a decoy
	set_pal_entry 0, 7, $e3, $00	; ink 7: magenta, a decoy
	set_pal_entry 0, 18, $03, $01	; paper 2: blue
	set_pal_entry 0, 23, $1c, $00	; paper 7, the border: green

	pass
