; lores 16c transparency: every nibble = 1, palette entry 1's hi matches
; nr_14 -> every pixel transparent -> whole display area shows black fallback.

	include "common.inc"

	silence_ula	; nr_14 := $e3
	set_pal_entry 0, 1, $e3, $00
	set_nr $6a, $20
	set_nr $15, $80

	ld hl, $4000
	ld de, $4001
	ld bc, 6143
	ld (hl), $11
	ldir

	pass
